/* SPDX-License-Identifier: MIT
 * Unit tests for the core library and theme engine.
 */
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <unistd.h>

#include <crypt.h>
#include <langinfo.h>
#include <locale.h>
#include <monetary.h>
#include <poll.h>
#include <sys/wait.h>

#include <fontconfig/fontconfig.h>

#include "../src/admin/admin.h"
#include "../src/appwiz/programs.h"
#include "../src/apps/runcmd.h"
#include "../src/power/schemes.h"
#include "../src/printers/printlib.h"
#include "../src/scanners/scanlib.h"
#include "../src/bluetooth/btinfo.h"
#include "../src/joy/joyinfo.h"
#include "../src/netconn/netconn.h"
#include "../src/intl/intl.h"
#include "../src/keyboard/layouts.h"
#include "../src/mouse/cursors.h"
#include "../src/files/columns.h"
#include "../src/files/favorites.h"
#include "../src/files/launcher.h"
#include "../src/files/names.h"
#include "../src/folders/filetypes.h"
#include "../src/fonts/fontlist.h"
#include "../src/schedtasks/task.h"
#include "../src/security/passwd.h"
#include "../src/saver/savers.h"
#include "../src/settings/gtktheme.h"
#include "../src/settings/qtpalette.h"
#include "../src/settings/settings.h"
#include "../src/settings/sounds.h"
#include "../src/settings/themes.h"
/* For GSettings in memory, never the user's dconf. */
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <json-glib/json-glib.h>
#include "../src/shell/apps.h"
#include "../src/startmenu/searchlib.h"
#include "../src/taskmgr/netstat.h"
#include "../src/shell/jumplist.h"
#include "../src/ui/textedit.h"
#include "../src/users/users.h"
#include "../src/theme/theme.h"
#include "../src/thumbnailer/thumbcache.h"

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

static bool load_shipped_scheme(const char *stem, xd_scheme *s) {
    xd_strbuf path = {0};
    xd_strbuf_appendf(&path, "%s/data/schemes/%s.ini", XD_SOURCE_ROOT, stem);
    xd_strbuf err = {0};
    bool ok = xd_scheme_load(xd_strbuf_cstr(&path), s, &err);
    if (!ok)
        fprintf(stderr, "%s: %s\n", stem, xd_strbuf_cstr(&err));
    xd_strbuf_free(&err);
    xd_strbuf_free(&path);
    return ok;
}

static bool rgb_is(xd_rgb c, uint8_t r, uint8_t g, uint8_t b) {
    return c.r == r && c.g == g && c.b == b;
}

static void test_shipped_schemes(void) {
    /* Every shipped scheme parses and has its own name. */
    xd_strbuf dir = {0};
    xd_strbuf_appendf(&dir, "%s/data/schemes", XD_SOURCE_ROOT);
    GDir *d = g_dir_open(xd_strbuf_cstr(&dir), 0, nullptr);
    CHECK(d != nullptr);
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    size_t count = 0;
    const char *file;
    while (d != nullptr && (file = g_dir_read_name(d)) != nullptr) {
        if (!g_str_has_suffix(file, ".ini"))
            continue;
        char *stem = g_strndup(file, strlen(file) - 4);
        xd_scheme s;
        CHECK(load_shipped_scheme(stem, &s));
        CHECK(g_hash_table_add(names, g_strdup(s.name)));
        g_free(stem);
        count++;
    }
    if (d != nullptr)
        g_dir_close(d);
    g_hash_table_destroy(names);
    xd_strbuf_free(&dir);
    /* XP's 22 classic schemes, Windows XP style's three colours and
     * Royale's Energy Blue. */
    CHECK(count == 26);

    /* Spot checks against the reference (Windows XP's registry values). */
    static const struct {
        const char *stem, *name;
        xd_syscolor color;
        uint8_t r, g, b;
        xd_luna_color luna;
        bool high_contrast;
    } want[] = {
        {.stem = "windows-standard", .name = "Windows Standard",
         .color = XD_COLOR_ACTIVE_TITLE, .r = 10, .g = 36, .b = 106},
        {.stem = "windows-classic", .name = "Windows Classic",
         .color = XD_COLOR_GRADIENT_INACTIVE_TITLE, .r = 181, .g = 181, .b = 181},
        {.stem = "brick", .name = "Brick", .color = XD_COLOR_BACKGROUND, .r = 66, .g = 0, .b = 0},
        {.stem = "brick", .name = "Brick",
         .color = XD_COLOR_MENU_HILIGHT, .r = 141, .g = 137, .b = 97},
        {.stem = "desert", .name = "Desert",
         .color = XD_COLOR_ACTIVE_TITLE, .r = 0, .g = 128, .b = 128},
        {.stem = "eggplant", .name = "Eggplant",
         .color = XD_COLOR_BUTTON_FACE, .r = 144, .g = 176, .b = 168},
        {.stem = "marine", .name = "Marine (high color)",
         .color = XD_COLOR_ACTIVE_TITLE, .r = 0, .g = 0, .b = 128},
        {.stem = "pumpkin", .name = "Pumpkin (large)",
         .color = XD_COLOR_ACTIVE_TITLE, .r = 215, .g = 165, .b = 47},
        {.stem = "red-white-blue", .name = "Red, White, and Blue (VGA)",
         .color = XD_COLOR_ACTIVE_TITLE, .r = 128, .g = 0, .b = 0},
        {.stem = "spruce", .name = "Spruce",
         .color = XD_COLOR_BUTTON_FACE, .r = 162, .g = 200, .b = 169},
        {.stem = "teal", .name = "Teal (VGA)",
         .color = XD_COLOR_BACKGROUND, .r = 0, .g = 64, .b = 64},
        {.stem = "high-contrast-1", .name = "High Contrast #1",
         .color = XD_COLOR_WINDOW_TEXT, .r = 255, .g = 255, .b = 0, .high_contrast = true},
        {.stem = "high-contrast-2", .name = "High Contrast #2",
         .color = XD_COLOR_WINDOW_TEXT, .r = 0, .g = 255, .b = 0, .high_contrast = true},
        {.stem = "high-contrast-white", .name = "High Contrast White",
         .color = XD_COLOR_GRAY_TEXT, .r = 0, .g = 255, .b = 0, .high_contrast = true},
        {.stem = "luna-blue", .name = "Default (blue)",
         .color = XD_COLOR_BUTTON_FACE, .r = 236, .g = 233, .b = 216, .luna = XD_LUNA_BLUE},
        {.stem = "luna-olive", .name = "Olive Green",
         .color = XD_COLOR_ACTIVE_TITLE, .r = 139, .g = 161, .b = 105, .luna = XD_LUNA_OLIVE},
        {.stem = "luna-silver", .name = "Silver",
         .color = XD_COLOR_BUTTON_FACE, .r = 224, .g = 223, .b = 227, .luna = XD_LUNA_SILVER},
        {.stem = "luna-energyblue", .name = "Energy Blue",
         .color = XD_COLOR_BUTTON_FACE, .r = 235, .g = 233, .b = 237, .luna = XD_LUNA_ENERGY},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(want); i++) {
        xd_scheme s;
        CHECK(load_shipped_scheme(want[i].stem, &s));
        CHECK(strcmp(s.name, want[i].name) == 0);
        CHECK(rgb_is(s.colors[want[i].color], want[i].r, want[i].g, want[i].b));
        CHECK(s.luna_color == want[i].luna);
        CHECK(s.high_contrast == want[i].high_contrast);
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

/* Each Luna variant draws its own measured caption (row 8 is its body
 * colour), with a red Close button; Blue is drawn exactly as measured. */
static void test_luna_variants(void) {
    static const struct {
        const char *stem;
        uint32_t caption; /* the caption's body colour */
    } variants[] = {
        {"luna-blue", 0x0054E3},
        {"luna-olive", 0xA7B580},
        {"luna-silver", 0xACABC4},
        {"luna-energyblue", 0x6DAAE9},
    };
    uint32_t close[4], minimize[4];
    for (size_t i = 0; i < XD_ARRAY_LEN(variants); i++) {
        xd_scheme scheme;
        CHECK(load_shipped_scheme(variants[i].stem, &scheme));
        xd_theme t;
        xd_theme_init(&t, XD_STYLE_LUNA, &scheme, 1.0);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 200);
        cairo_t *cr = cairo_create(surf);
        xd_frame f = {.title = "", .active = true, .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MINIMIZE};
        xd_rect outer = {0, 0, 300, 200};
        xd_theme_draw_frame(&t, cr, outer, &f);
        /* Row 8 of the measured caption is ActiveTitle. */
        CHECK(near(pixel(surf, 200, 8), variants[i].caption, 0));
        xd_frame_layout l;
        xd_theme_frame_layout(&t, outer, &f, &l);
        for (size_t b = 0; b < l.button_count; b++) {
            uint32_t px = pixel(surf, l.button_rects[b].x + 3, l.button_rects[b].y + 10);
            if (l.button_parts[b] == XD_PART_CLOSE)
                close[i] = px;
            else
                minimize[i] = px;
        }
        cairo_destroy(cr);
        cairo_surface_destroy(surf);
    }
    CHECK(minimize[1] != minimize[0] && minimize[2] != minimize[0] && minimize[3] != minimize[0]);
    for (size_t i = 0; i < 4; i++)
        CHECK(((close[i] >> 16) & 0xFF) > ((close[i] >> 8) & 0xFF) + 64);

    /* Silver is drawn as measured off XP: a glossy caption from a light
     * top through lavender grey A4A3BE to white at its foot, a light
     * silver taskbar and the green Start button. */
    xd_scheme sv;
    CHECK(load_shipped_scheme("luna-silver", &sv));
    xd_theme st;
    xd_theme_init(&st, XD_STYLE_LUNA, &sv, 1.0);
    cairo_surface_t *ss = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 100);
    cairo_t *scr = cairo_create(ss);
    xd_frame sf = {.title = "", .active = true, .buttons = XD_CAPBTN_CLOSE};
    xd_theme_draw_frame(&st, scr, (xd_rect){0, 0, 300, 60}, &sf);
    CHECK(near(pixel(ss, 150, 2), 0xFFFFFF, 0));  /* the gloss */
    CHECK(near(pixel(ss, 150, 7), 0xA4A3BE, 0));  /* the darkest row */
    CHECK(near(pixel(ss, 150, 26), 0xFFFFFF, 0)); /* the caption's foot */
    CHECK(near(pixel(ss, 150, 29), 0x777792, 0)); /* the line above the client */
    sf.active = false;
    xd_theme_draw_frame(&st, scr, (xd_rect){0, 0, 300, 60}, &sf);
    CHECK(near(pixel(ss, 150, 8), 0xD9D9E5, 0));
    xd_theme_draw_taskbar(&st, scr, (xd_rect){0, 60, 300, 30}, XD_EDGE_BOTTOM);
    CHECK(near(pixel(ss, 150, 62), 0xFFFFFF, 0) && near(pixel(ss, 150, 65), 0x9A99B1, 0));
    xd_theme_draw_start_button(&st, scr, (xd_rect){0, 60, 99, 30}, 0);
    uint32_t green = pixel(ss, 4, 75);
    CHECK(((green >> 8) & 0xFF) > ((green >> 16) & 0xFF) + 40);
    /* Olive Green, measured too; an inactive caption, which its reference
     * doesn't show, is the active one faded: lighter and greyer. */
    xd_scheme ol;
    CHECK(load_shipped_scheme("luna-olive", &ol));
    xd_theme_init(&st, XD_STYLE_LUNA, &ol, 1.0);
    sf.active = true;
    xd_theme_draw_frame(&st, scr, (xd_rect){0, 0, 300, 60}, &sf);
    uint32_t on = pixel(ss, 150, 8);
    sf.active = false;
    xd_theme_draw_frame(&st, scr, (xd_rect){0, 0, 300, 60}, &sf);
    uint32_t off = pixel(ss, 150, 8);
    CHECK(on == 0xA7B580 && (off & 0xFF) > (on & 0xFF) + 30);
    CHECK(((off >> 8) & 0xFF) - (off & 0xFF) < ((on >> 8) & 0xFF) - (on & 0xFF));
    xd_theme_draw_task_button(&st, scr, (xd_rect){0, 60, 160, 25}, XD_STATE_PRESSED, "", nullptr);
    CHECK(near(pixel(ss, 80, 70), 0x8D977E, 0));

    /* The XP Start menu takes each variant's colours: Blue's own, Energy
     * Blue's measured navy program list and blue column. */
    xd_luna_start_colors sc;
    xd_scheme bl;
    CHECK(load_shipped_scheme("luna-blue", &bl));
    xd_theme_init(&st, XD_STYLE_LUNA, &bl, 1.0);
    xd_theme_luna_start_colors(&st, &sc);
    CHECK(sc.left_bg == 0xFFFFFF && sc.right_bg == 0xD3E5FA && sc.head_a == 0x3D85EE && sc.accent);
    xd_scheme eb;
    CHECK(load_shipped_scheme("luna-energyblue", &eb));
    xd_theme_init(&st, XD_STYLE_LUNA, &eb, 1.0);
    xd_theme_luna_start_colors(&st, &sc);
    CHECK(sc.left_bg == 0x10377C && sc.right_bg == 0x5373BF && sc.left_text == 0xFFFFFF &&
          !sc.accent);
    xd_theme_draw_taskbar(&st, scr, (xd_rect){0, 60, 300, 30}, XD_EDGE_BOTTOM);
    CHECK(near(pixel(ss, 150, 61), 0x99C6F1, 0));
    cairo_destroy(scr);
    cairo_surface_destroy(ss);

    /* The GTK theme follows the variant's selection colour. */
    xd_scheme olive;
    CHECK(load_shipped_scheme("luna-olive", &olive));
    xd_theme t;
    xd_theme_init(&t, XD_STYLE_LUNA, &olive, 1.0);
    xd_strbuf css = {0};
    xd_theme_gtk_css(&t, &css);
    CHECK(strstr(xd_strbuf_cstr(&css), "@define-color theme_selected_bg_color #93a070;") !=
          nullptr);
    xd_strbuf_free(&css);

    /* A bad LunaColor is refused. */
    xd_scheme s;
    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=homestead\n"), &s, nullptr));
    CHECK(s.luna_color == XD_LUNA_OLIVE);
    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=EnergyBlue\n"), &s, nullptr));
    CHECK(s.luna_color == XD_LUNA_ENERGY);
    CHECK(!xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=Green\n"), &s, nullptr));
    CHECK(s.luna_color == XD_LUNA_NONE);
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

/* Writes `text` to root/rel, making its directories. */
static void put_file(const char *root, const char *rel, const char *text) {
    char *path = g_build_filename(root, rel, nullptr);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    CHECK(g_file_set_contents(path, text, -1, nullptr));
    g_free(dir);
    g_free(path);
}

static void rm_tree(const char *path) {
    GDir *d = g_dir_open(path, 0, nullptr);
    if (d != nullptr) {
        const char *e;
        while ((e = g_dir_read_name(d)) != nullptr) {
            char *child = g_build_filename(path, e, nullptr);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                rm_tree(child);
            else
                CHECK(g_remove(child) == 0);
            g_free(child);
        }
        g_dir_close(d);
    }
    CHECK(g_rmdir(path) == 0);
}

static const xd_installed_theme *theme_at(GPtrArray *list, guint i) {
    return i < list->len ? g_ptr_array_index(list, i) : nullptr;
}

static void test_theme_settings(void) {
    /* [Appearance] IconTheme and GtkTheme: directory names, "" = default. */
    xd_settings a;
    xd_settings_parse(XD_STR("[Appearance]\nIconTheme=oxygen\nGtkTheme=Adwaita:dark\n"), &a);
    CHECK(strcmp(a.icon_theme, "oxygen") == 0 && strcmp(a.gtk_theme, "Adwaita:dark") == 0);
    CHECK(strcmp(xd_settings_icon_theme(&a), "oxygen") == 0);
    CHECK(strcmp(xd_settings_gtk_theme(&a), "Adwaita:dark") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && strcmp(b.icon_theme, "oxygen") == 0);
    xd_strbuf_free(&text);
    /* No paths, nothing hidden, nothing too long. */
    xd_settings_parse(XD_STR("[Appearance]\nIconTheme=../../etc\nGtkTheme=.hidden\n"), &a);
    CHECK(a.icon_theme[0] == '\0' && a.gtk_theme[0] == '\0');
    xd_settings_parse(XD_STR("[Appearance]\nIconTheme=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"),
                      &a);
    CHECK(a.icon_theme[0] == '\0');
    /* Unset: ours, and $XD_ICON_THEME before breeze. */
    xd_settings_defaults(&a);
    CHECK(strcmp(xd_settings_gtk_theme(&a), "XeonDesktop") == 0);
    char *env = g_strdup(g_getenv("XD_ICON_THEME"));
    g_unsetenv("XD_ICON_THEME");
    CHECK(strcmp(xd_settings_icon_theme(&a), "breeze") == 0);
    g_setenv("XD_ICON_THEME", "Adwaita", TRUE);
    CHECK(strcmp(xd_settings_icon_theme(&a), "Adwaita") == 0);
    snprintf(a.icon_theme, sizeof(a.icon_theme), "%s", "oxygen");
    CHECK(strcmp(xd_settings_icon_theme(&a), "oxygen") == 0); /* the setting wins */
    if (env != nullptr)
        g_setenv("XD_ICON_THEME", env, TRUE);
    else
        g_unsetenv("XD_ICON_THEME");
    g_free(env);
    /* A dialog's Apply carries them like any other key. */
    xd_settings base, pending, now;
    xd_settings_defaults(&base);
    pending = base;
    snprintf(pending.gtk_theme, sizeof(pending.gtk_theme), "%s", "Breeze");
    now = base;
    now.style = XD_STYLE_LUNA;
    xd_settings_merge(&base, &pending, &now);
    CHECK(strcmp(now.gtk_theme, "Breeze") == 0 && now.style == XD_STYLE_LUNA);

    /* Discovery, in a scratch tree laid out like two $XDG_DATA_DIRS. */
    char *root = g_dir_make_tmp("xd-themes-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    /* one: "zed" (full), "cur" (cursors only, no Directories), "cur2"
     * (Directories=cursors), "hid" (Hidden), "Adwaita" (Hidden, listed
     * anyway), "noindex", "default"; two: another "zed" (hidden by
     * one's), "Alpha" (no Name). */
    put_file(root, "one/icons/zed/index.theme",
             "[Icon Theme]\nName=Zed Icons\nDirectories=16x16/apps\n"
             "[16x16/apps]\nSize=16\n");
    put_file(root, "one/icons/cur/index.theme", "[Icon Theme]\nName=Cursors\n");
    put_file(root, "one/icons/cur2/index.theme",
             "[Icon Theme]\nName=Cursors 2\nDirectories=cursors\n");
    put_file(root, "one/icons/hid/index.theme",
             "[Icon Theme]\nName=Hid\nHidden=true\nDirectories=16x16/apps\n");
    put_file(root, "one/icons/Adwaita/index.theme",
             "[Icon Theme]\nName=Adwaita\nHidden=true\nDirectories=16x16/apps\n");
    put_file(root, "one/icons/noindex/16x16/apps/x.png", "");
    put_file(root, "one/icons/default/index.theme",
             "[Icon Theme]\nInherits=zed\nDirectories=16x16/apps\n");
    put_file(root, "two/icons/zed/index.theme",
             "[Icon Theme]\nName=Other Zed\nDirectories=16x16/apps\n");
    put_file(root, "two/icons/Alpha/index.theme",
             "[Icon Theme]\nScaledDirectories=scalable/apps\n");
    /* GTK: "Zap" (gtk-3.0), "Old" (gtk-2.0 only), our generated theme,
     * and a "Breeze" in both. */
    put_file(root, "one/themes/Zap/gtk-3.0/gtk.css", "");
    put_file(root, "one/themes/Old/gtk-2.0/gtkrc", "");
    put_file(root, "one/themes/XeonDesktop/gtk-3.0/gtk.css", "");
    put_file(root, "one/themes/XeonDesktop-alt/gtk-3.0/gtk.css", "");
    put_file(root, "one/themes/Breeze/gtk-3.0/gtk.css", "");
    put_file(root, "two/themes/Breeze/gtk-3.0/gtk.css", "");

    char *one = g_build_filename(root, "one", "icons", nullptr);
    char *two = g_build_filename(root, "two", "icons", nullptr);
    const char *icon_dirs[] = {one, two, "/nonexistent", nullptr};
    GPtrArray *icons = xd_icon_themes_list(icon_dirs);
    CHECK(icons->len == 3);
    CHECK(theme_at(icons, 0) && strcmp(theme_at(icons, 0)->dir, "Adwaita") == 0);
    CHECK(theme_at(icons, 1) && strcmp(theme_at(icons, 1)->dir, "Alpha") == 0 &&
          strcmp(theme_at(icons, 1)->name, "Alpha") == 0);
    CHECK(theme_at(icons, 2) && strcmp(theme_at(icons, 2)->dir, "zed") == 0 &&
          strcmp(theme_at(icons, 2)->name, "Zed Icons") == 0);
    g_ptr_array_unref(icons);
    g_free(one);
    g_free(two);

    one = g_build_filename(root, "one", "themes", nullptr);
    two = g_build_filename(root, "two", "themes", nullptr);
    const char *gtk_dirs[] = {one, two, nullptr};
    GPtrArray *gtk = xd_gtk_themes_list(gtk_dirs);
    static const char *const want[] = {"Adwaita", "Breeze", "HighContrast", "Zap"};
    CHECK(gtk->len == G_N_ELEMENTS(want));
    for (guint i = 0; i < G_N_ELEMENTS(want); i++)
        CHECK(theme_at(gtk, i) && strcmp(theme_at(gtk, i)->dir, want[i]) == 0);
    g_ptr_array_unref(gtk);
    g_free(one);
    g_free(two);

    char **search = xd_icon_theme_dirs();
    CHECK(g_strv_length(search) >= 3 && g_str_has_suffix(search[1], "/.icons"));
    g_strfreev(search);
    search = xd_gtk_theme_dirs();
    CHECK(g_strv_length(search) >= 3 && g_str_has_suffix(search[1], "/.themes"));
    g_strfreev(search);

    /* settings.ini: the user's other keys and comments stay. */
    char *config = g_build_filename(root, "config", nullptr);
    put_file(config, "gtk-3.0/settings.ini",
             "# mine\n[Settings]\ngtk-font-name=Sans 10\ngtk-theme-name=Old\n");
    put_file(config, "gtk-4.0/settings.ini", "[Settings]\ngtk-theme-name=Old\n");
    GSettingsBackend *backend = g_memory_settings_backend_new();
    xd_gtk_settings_export(config, backend, "Zap", "zed");
    char *ini3 = g_build_filename(config, "gtk-3.0", "settings.ini", nullptr);
    char *ini4 = g_build_filename(config, "gtk-4.0", "settings.ini", nullptr);
    GKeyFile *kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini3, G_KEY_FILE_KEEP_COMMENTS, nullptr));
    char *v = g_key_file_get_string(kf, "Settings", "gtk-theme-name", nullptr);
    CHECK(g_strcmp0(v, "Zap") == 0);
    g_free(v);
    v = g_key_file_get_string(kf, "Settings", "gtk-icon-theme-name", nullptr);
    CHECK(g_strcmp0(v, "zed") == 0);
    g_free(v);
    v = g_key_file_get_string(kf, "Settings", "gtk-font-name", nullptr);
    CHECK(g_strcmp0(v, "Sans 10") == 0);
    g_free(v);
    v = g_key_file_get_comment(kf, nullptr, nullptr, nullptr);
    CHECK(v != nullptr && strstr(v, "mine") != nullptr);
    g_free(v);
    g_key_file_free(kf);
    /* Ours: GTK4 has no such theme, so its file loses the key. */
    xd_gtk_settings_export(config, backend, "XeonDesktop", nullptr);
    kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini4, G_KEY_FILE_NONE, nullptr));
    CHECK(!g_key_file_has_key(kf, "Settings", "gtk-theme-name", nullptr));
    v = g_key_file_get_string(kf, "Settings", "gtk-icon-theme-name", nullptr);
    CHECK(g_strcmp0(v, "zed") == 0);
    g_free(v);
    g_key_file_free(kf);
    kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini3, G_KEY_FILE_NONE, nullptr));
    v = g_key_file_get_string(kf, "Settings", "gtk-theme-name", nullptr);
    CHECK(g_strcmp0(v, "XeonDesktop") == 0);
    g_free(v);
    g_key_file_free(kf);
    /* GSettings, where the schema is installed (in memory here). */
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema =
        src != nullptr ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE)
                       : nullptr;
    if (schema != nullptr) {
        GSettings *s = g_settings_new_full(schema, backend, nullptr);
        v = g_settings_get_string(s, "gtk-theme");
        CHECK(strcmp(v, "XeonDesktop") == 0);
        g_free(v);
        v = g_settings_get_string(s, "icon-theme");
        CHECK(strcmp(v, "zed") == 0);
        g_free(v);
        /* After a rewrite, ours switches between its two names (GTK only
         * reloads a theme when the name changes)... */
        CHECK(xd_gtk_theme_nudge(backend));
        v = g_settings_get_string(s, "gtk-theme");
        CHECK(strcmp(v, XD_GTK_THEME_ALT_NAME) == 0);
        g_free(v);
        CHECK(xd_gtk_theme_nudge(backend));
        v = g_settings_get_string(s, "gtk-theme");
        CHECK(strcmp(v, XD_GTK_THEME_NAME) == 0);
        g_free(v);
        /* ...but another theme set there stays. */
        g_settings_set_string(s, "gtk-theme", "Zap");
        CHECK(!xd_gtk_theme_nudge(backend));
        v = g_settings_get_string(s, "gtk-theme");
        CHECK(strcmp(v, "Zap") == 0);
        g_free(v);
        g_object_unref(s);
        /* Nothing set at all: ours goes in. */
        GSettingsBackend *fresh = g_memory_settings_backend_new();
        CHECK(xd_gtk_theme_nudge(fresh));
        s = g_settings_new_full(schema, fresh, nullptr);
        v = g_settings_get_string(s, "gtk-theme");
        CHECK(strcmp(v, XD_GTK_THEME_NAME) == 0);
        g_free(v);
        g_object_unref(s);
        g_object_unref(fresh);
        g_settings_schema_unref(schema);
    }
    g_object_unref(backend);
    /* Removing from a file that isn't there creates nothing. */
    char *none = g_build_filename(config, "gtk-5.0", "settings.ini", nullptr);
    CHECK(xd_gtk_ini_set(none, "gtk-theme-name", nullptr));
    CHECK(!g_file_test(none, G_FILE_TEST_EXISTS));
    g_free(none);
    g_free(ini3);
    g_free(ini4);
    g_free(config);
    rm_tree(root);
    g_free(root);
}

/* Aero's Window Color and Appearance: the settings, and the glass they
 * give the frames, taskbar and Start menu. */
static bool near0(double d) { return d < 1e-9 && d > -1e-9; }

static void test_glass_color(void) {
    xd_settings s;
    xd_settings_defaults(&s);
    CHECK(s.glass_color == XD_GLASS_DEFAULT && s.glass_transparency);
    xd_settings_parse(XD_STR("[Appearance]\nGlassColor=a8ce0f0f\nGlassTransparency=false\n"), &s);
    CHECK(s.glass_color == 0xA8CE0F0Fu && !s.glass_transparency);
    /* Not 8 hex digits: ignored. */
    xd_settings_parse(XD_STR("[Appearance]\nGlassColor=0xA8CE0F\n"), &s);
    CHECK(s.glass_color == XD_GLASS_DEFAULT);
    xd_settings_parse(XD_STR("[Appearance]\nGlassColor=-1234567\n"), &s);
    CHECK(s.glass_color == XD_GLASS_DEFAULT);
    s.glass_color = 0x54FCFCFCu;
    s.glass_transparency = false;
    xd_strbuf text = {0};
    xd_settings_serialize(&s, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "GlassColor=54FCFCFC") != nullptr);
    xd_settings back;
    xd_settings_parse(xd_strbuf_view(&text), &back);
    CHECK(back.glass_color == 0x54FCFCFCu && !back.glass_transparency);
    xd_strbuf_free(&text);
    xd_scheme scheme;
    xd_settings_scheme(&back, &scheme);
    CHECK(scheme.glass_color == 0x54FCFCFCu && scheme.glass_opaque);

    /* HSV round trip, greys included. */
    static const uint32_t rgbs[] = {0x74B8FC, 0xCE0F0F, 0x555555, 0x14A600, 0xFF0099, 0};
    for (size_t i = 0; i < XD_ARRAY_LEN(rgbs); i++) {
        xd_rgba c = xd_rgba_hex(rgbs[i]);
        xd_rgba back_c = xd_rgba_from_hsv(xd_rgba_to_hsv(c), 0.5);
        CHECK(near0(back_c.r - c.r) && near0(back_c.g - c.g) &&
              near0(back_c.b - c.b) && back_c.a == 0.5);
    }
    CHECK(near0(xd_rgba_to_hsv(xd_rgba_hex(0x0000FF)).h - 240));

    /* Sixteen named swatches, Sky first and the default. */
    CHECK(strcmp(xd_glass_swatches[0].name, "Sky") == 0 &&
          xd_glass_swatches[0].argb == XD_GLASS_DEFAULT);
    CHECK(strcmp(xd_glass_swatches[XD_GLASS_SWATCH_COUNT - 1].name, "Frost") == 0);

    /* Opaque glass: the colour in light grey by its intensity. */
    xd_scheme_init_default(&scheme);
    scheme.glass_color = 0x80FF0000u;
    xd_theme t;
    xd_theme_init(&t, XD_STYLE_AERO, &scheme, 1.0);
    t.glass = false;
    xd_glass_colors g;
    xd_theme_glass_colors(&t, &g);
    double k = 0x80 / 255.0, grey = 0xEB / 255.0;
    CHECK(!g.translucent && g.frame.a == 1.0 && near0(g.frame.r - (grey + (1 - grey) * k)) &&
          near0(g.frame.g - grey * (1 - k)));
    /* Translucent with blur and transparency on: more intensity, more colour. */
    t.glass = true;
    xd_theme_glass_colors(&t, &g);
    CHECK(g.translucent && g.frame.a < 1.0 && g.frame.r == 1.0 && g.frame.g == 0.0);
    double low = g.frame.a;
    t.scheme.glass_color = 0xC0FF0000u;
    xd_theme_glass_colors(&t, &g);
    CHECK(g.frame.a > low && g.taskbar.a > 0 && g.taskbar.r > g.taskbar.g);
    /* Transparency off: opaque even with blur. */
    t.scheme.glass_opaque = true;
    xd_theme_glass_colors(&t, &g);
    CHECK(!g.translucent && g.frame.a == 1.0 && g.taskbar.a == 1.0 && g.menu.a == 1.0);

    /* The GTK theme's title bars follow the colour. */
    xd_strbuf sky = {0}, ruby = {0};
    xd_scheme_init_default(&scheme);
    xd_theme_init(&t, XD_STYLE_AERO, &scheme, 1.0);
    xd_theme_gtk_css(&t, &sky);
    t.scheme.glass_color = 0xA8CE0F0Fu;
    xd_theme_gtk_css(&t, &ruby);
    CHECK(!xd_str_eq(xd_strbuf_view(&sky), xd_strbuf_view(&ruby)));
    xd_toolkit_colors tc;
    xd_theme_toolkit_colors(&t, &tc);
    CHECK(tc.title_bg.r > tc.title_bg.g + 0.4 && tc.title_bg2.r > tc.title_bg2.b);
    xd_strbuf_free(&sky);
    xd_strbuf_free(&ruby);
}

static void test_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    CHECK(a.style == XD_STYLE_CLASSIC && a.wallpaper_mode == XD_WALLPAPER_FILL);

    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nScheme=windows-classic\n"
                             "FontSize=large\nFontSmoothing=cleartype\nFuture=ignored\n"
                             "[Desktop]\nWallpaper=/w/p.jpg\nWallpaperMode=tile\n"
                             "[Effects]\nLargeIcons=true\nMenuTransition=Scroll\n"
                             "FullWindowDrag=false\n[ScreenSaver]\nWait=7\n"
                             "[SomethingNew]\nKey=v\n"),
                      &a);
    CHECK(a.style == XD_STYLE_LUNA && strcmp(a.scheme, "windows-classic") == 0);
    CHECK(a.font_size == XD_FONT_LARGE && a.font_smoothing == XD_SMOOTH_CLEARTYPE);
    CHECK(strcmp(a.wallpaper, "/w/p.jpg") == 0 && a.wallpaper_mode == XD_WALLPAPER_TILE);
    CHECK(a.large_icons && a.screensaver_wait == 7);
    CHECK(a.menu_transition == XD_TRANSITION_SCROLL && !a.full_window_drag);

    /* Serialize -> parse round trip. */
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b));
    b.large_icons = false;
    CHECK(!xd_settings_equal(&a, &b));
    b.large_icons = true;
    b.menu_transition = XD_TRANSITION_FADE;
    CHECK(!xd_settings_equal(&a, &b));
    xd_strbuf_free(&text);

    /* Bad values keep defaults; a scheme can't be a path. */
    xd_settings_parse(XD_STR("[Appearance]\nStyle=bogus\nScheme=../../etc/x\n"
                             "[Desktop]\nWallpaper=relative.jpg\nWallpaperMode=x\n"
                             "[Effects]\nMenuTransition=wipe\n[ScreenSaver]\nWait=0\n"),
                      &a);
    CHECK(a.style == XD_STYLE_CLASSIC && a.scheme[0] == '\0' && a.wallpaper[0] == '\0');
    CHECK(a.menu_transition == XD_TRANSITION_FADE && a.full_window_drag);
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

    /* A dialog's Apply keeps what others saved since it loaded the file. */
    xd_settings base;
    xd_settings_defaults(&base);
    xd_settings_set_tray_rule(&base, "nm-applet", XD_TRAY_ALWAYS_SHOW);
    xd_settings pending = base;
    pending.show_clock = false;
    xd_settings_set_tray_rule(&pending, "nm-applet", XD_TRAY_AUTO);
    xd_settings now = base;
    now.style = XD_STYLE_LUNA;
    snprintf(now.wallpaper, sizeof(now.wallpaper), "%s", "/w/new.jpg");
    now.show_clock = true;
    xd_settings_merge(&base, &pending, &now);
    CHECK(now.style == XD_STYLE_LUNA && strcmp(now.wallpaper, "/w/new.jpg") == 0);
    CHECK(!now.show_clock && xd_settings_tray_rule(&now, "nm-applet") == XD_TRAY_AUTO);

    /* Customize Start Menu and Quick Launch's View menu. */
    xd_settings_defaults(&a);
    CHECK(a.start_large_icons && a.start_programs == 6 && a.start_highlight_new);
    CHECK(!a.quick_large && !a.quick_show_text && !a.quick_show_title);
    xd_settings_parse(XD_STR("[StartMenu]\nLargeIcons=false\nPrograms=12\n"
                             "HighlightNewPrograms=false\n[Taskbar]\nQuickLaunchLargeIcons=true\n"
                             "QuickLaunchShowText=true\nQuickLaunchShowTitle=true\n"),
                      &a);
    CHECK(!a.start_large_icons && a.start_programs == 12 && !a.start_highlight_new);
    CHECK(a.quick_large && a.quick_show_text && a.quick_show_title);
    xd_settings_serialize(&a, &text);
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && b.start_programs == 12 && b.quick_show_title);
    xd_strbuf_free(&text);
    xd_settings_parse(XD_STR("[StartMenu]\nPrograms=31\n"), &a);
    CHECK(a.start_programs == 6);
}

static void test_saver_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    const xd_saver_settings *sv = &a.savers;
    CHECK(strcmp(sv->marquee.text, "XeonDesktop") == 0 && sv->marquee.random_position);
    CHECK(sv->starfield.density == 200 && sv->mystify.clear_screen && sv->beziers.curves == 1);
    CHECK(!sv->pipes.multiple && sv->pipes.joints == XD_PIPES_MIXED);
    CHECK(sv->text3d.surface == XD_TEXT3D_REFLECTION && sv->text3d.rotation == XD_TEXT3D_SPIN);

    xd_settings_parse(XD_STR("[ScreenSaver.Marquee]\nText=Gone fishing\nFont=DejaVu Serif\n"
                             "FontSize=36\nBold=true\nTextColor=255 255 0\n"
                             "BackgroundColor=0  0 128\nPosition=centered\nSpeed=8\n"
                             "[ScreenSaver.Starfield]\nWarpSpeed=10\nDensity=50\n"
                             "[ScreenSaver.Mystify]\nPolygon2Active=false\nPolygon1Lines=3\n"
                             "Polygon1Colors=two\nPolygon1Color2=0 255 0\nClearScreen=false\n"
                             "[ScreenSaver.Beziers]\nCurves=4\nLines=40\nSpeed=0\n"
                             "[ScreenSaver.Flying]\nWarpSpeed=2\nDensity=120\n"
                             "[ScreenSaver.Pipes]\nPipes=multiple\nJoints=cycle\n"
                             "[screensaver.text3d]\nDisplay=time\nText=Hi\nRotation=wobble\n"
                             "Speed=3\nSize=9\nSurface=solid\nColor=0 128 128\n"),
                      &a);
    CHECK(strcmp(sv->marquee.text, "Gone fishing") == 0 &&
          strcmp(sv->marquee.font.family, "DejaVu Serif") == 0);
    CHECK(sv->marquee.font.size_pt == 36 && sv->marquee.font.bold && !sv->marquee.font.italic);
    CHECK(sv->marquee.color.r == 255 && sv->marquee.color.g == 255 && sv->marquee.color.b == 0);
    CHECK(sv->marquee.background.b == 128 && !sv->marquee.random_position &&
          sv->marquee.speed == 8);
    CHECK(sv->starfield.speed == 10 && sv->starfield.density == 50);
    CHECK(sv->mystify.shapes[0].active && !sv->mystify.shapes[1].active);
    CHECK(sv->mystify.shapes[0].lines == 3 && sv->mystify.shapes[0].two_colors &&
          sv->mystify.shapes[0].colors[1].g == 255 && !sv->mystify.clear_screen);
    CHECK(sv->beziers.curves == 4 && sv->beziers.lines == 40 && sv->beziers.speed == 0);
    CHECK(sv->flying.speed == 2 && sv->flying.density == 120);
    CHECK(sv->pipes.multiple && sv->pipes.joints == XD_PIPES_CYCLE);
    CHECK(sv->text3d.show_time && strcmp(sv->text3d.text, "Hi") == 0);
    CHECK(sv->text3d.rotation == XD_TEXT3D_WOBBLE && sv->text3d.speed == 3 &&
          sv->text3d.size == 9 && sv->text3d.surface == XD_TEXT3D_SOLID &&
          sv->text3d.color.g == 128);

    /* Serialize -> parse round trip. */
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && memcmp(&a.savers, &b.savers, sizeof(a.savers)) == 0);
    xd_strbuf_free(&text);

    /* Out of range or malformed values keep the defaults. */
    xd_settings_parse(XD_STR("[ScreenSaver.Marquee]\nSpeed=11\nTextColor=256 0 0\n"
                             "BackgroundColor=1 2\nPosition=left\nText=a\tb\n"
                             "[ScreenSaver.Starfield]\nDensity=5\n"
                             "[ScreenSaver.Mystify]\nPolygon1Lines=16\nPolygon3Active=false\n"
                             "[ScreenSaver.Beziers]\nCurves=0\nLines=51\n"
                             "[ScreenSaver.Pipes]\nJoints=bogus\nPipes=many\n"
                             "[ScreenSaver.Text3D]\nRotation=tumble\nSize=-1\n"),
                      &a);
    xd_saver_settings def;
    xd_saver_settings_defaults(&def);
    CHECK(memcmp(&a.savers, &def, sizeof(def)) == 0);
}

/* Renders `frames` frames of a saver from `seed` into a small image. */
static cairo_surface_t *saver_frames(const char *id, const xd_saver_settings *o, uint64_t seed,
                                     int frames, double dt) {
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 160, 120);
    cairo_t *cr = cairo_create(img);
    xd_saver *sv = xd_saver_create_seeded(id, o, 160, 120, 0.5, seed);
    for (int i = 0; i < frames; i++)
        xd_saver_frame(sv, cr, dt);
    xd_saver_destroy(sv);
    cairo_destroy(cr);
    cairo_surface_flush(img);
    return img;
}

static bool same_pixels(cairo_surface_t *a, cairo_surface_t *b) {
    size_t n = (size_t)cairo_image_surface_get_stride(a) * (size_t)cairo_image_surface_get_height(a);
    return memcmp(cairo_image_surface_get_data(a), cairo_image_surface_get_data(b), n) == 0;
}

static void test_savers(void) {
    const xd_saver_info *list = nullptr;
    size_t n = xd_savers(&list);
    CHECK(n == 8 && strcmp(list[0].name, "3D Pipes") == 0 && strcmp(list[1].id, "text3d") == 0);
    CHECK(xd_saver_find("pipes") != nullptr && xd_saver_find("nope") == nullptr);
    CHECK(!xd_saver_animated("blank") && !xd_saver_has_settings("blank"));
    CHECK(xd_saver_has_settings("marquee") && xd_saver_create("nope", nullptr, 10, 10, 1) == nullptr);

    xd_settings st;
    xd_settings_defaults(&st);
    for (size_t i = 0; i < n; i++) {
        const char *id = list[i].id;
        /* The same seed and frame times give the same picture... */
        cairo_surface_t *a = saver_frames(id, &st.savers, 7, 40, 1.0 / 30);
        cairo_surface_t *b = saver_frames(id, &st.savers, 7, 40, 1.0 / 30);
        bool same = same_pixels(a, b);
        CHECK(same);
        if (!same)
            fprintf(stderr, "  saver %s isn't deterministic\n", id);
        /* ...and the picture moves on (Blank Screen's stays black). */
        cairo_surface_t *c = saver_frames(id, &st.savers, 7, 41, 1.0 / 30);
        bool moved = !same_pixels(a, c);
        CHECK(moved == xd_saver_animated(id));
        if (moved != xd_saver_animated(id))
            fprintf(stderr, "  saver %s: moved %d\n", id, moved);
        cairo_surface_destroy(a);
        cairo_surface_destroy(b);
        cairo_surface_destroy(c);
    }
    /* Where the random numbers show, another seed is another picture. */
    static const char *const random_ones[] = {"pipes", "beziers", "mystify", "starfield", "flying"};
    for (size_t i = 0; i < XD_ARRAY_LEN(random_ones); i++) {
        cairo_surface_t *a = saver_frames(random_ones[i], &st.savers, 7, 20, 1.0 / 30);
        cairo_surface_t *b = saver_frames(random_ones[i], &st.savers, 8, 20, 1.0 / 30);
        CHECK(!same_pixels(a, b));
        cairo_surface_destroy(a);
        cairo_surface_destroy(b);
    }

    /* Every option draws: 3D Pipes filling the screen several times over,
     * Mystify keeping its trails, 3D Text in each rotation and surface. */
    xd_saver_settings o = st.savers;
    o.pipes.multiple = true;
    o.pipes.joints = XD_PIPES_CYCLE;
    cairo_surface_destroy(saver_frames("pipes", &o, 3, 600, 0.25));
    o.mystify.clear_screen = false;
    o.mystify.shapes[0].two_colors = true;
    cairo_surface_t *m1 = saver_frames("mystify", &o, 3, 30, 1.0 / 30);
    cairo_surface_t *m2 = saver_frames("mystify", &o, 3, 34, 1.0 / 30);
    CHECK(!same_pixels(m1, m2));
    cairo_surface_destroy(m1);
    cairo_surface_destroy(m2);
    for (int r = 0; r < XD_TEXT3D_ROTATION_COUNT; r++) {
        o.text3d.rotation = (xd_text3d_rotation)r;
        o.text3d.surface = r % 2 == 0 ? XD_TEXT3D_SOLID : XD_TEXT3D_REFLECTION;
        cairo_surface_t *a = saver_frames("text3d", &o, 5, 15, 0.1);
        cairo_surface_t *b = saver_frames("text3d", &o, 5, 15, 0.1);
        CHECK(same_pixels(a, b));
        cairo_surface_destroy(a);
        cairo_surface_destroy(b);
    }
    o.text3d.show_time = true;
    cairo_surface_destroy(saver_frames("text3d", &o, 5, 3, 0.1));
    o.marquee.text[0] = '\0';
    cairo_surface_destroy(saver_frames("marquee", &o, 5, 3, 0.1));
}

static void test_desktop_icon_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    const xd_desktop_icon_settings *di = &a.desktop_icons;
    CHECK(di->documents && di->computer && di->network && !di->browser && di->show);
    CHECK(!di->auto_arrange && di->align_to_grid && xd_desk_icon_px(&a) == 32);
    /* XP's "Use large icons" makes the (medium) desktop icons 48. */
    a.large_icons = true;
    CHECK(xd_desk_icon_px(&a) == 48);
    a.desktop_icons.size = XD_DESK_ICONS_SMALL;
    CHECK(xd_desk_icon_px(&a) == 16);

    xd_settings_parse(XD_STR("[DesktopIcons]\nMyComputer=false\nWebBrowser=true\n"
                             "AutoArrange=true\nAlignToGrid=false\nSize=large\nShow=false\n"
                             "MyComputerIcon=/i/pc.png\nRecycleBinFullIcon=relative.png\n"),
                      &a);
    CHECK(!di->computer && di->browser && di->auto_arrange && !di->align_to_grid && !di->show);
    CHECK(di->size == XD_DESK_ICONS_LARGE && xd_desk_icon_px(&a) == 48);
    CHECK(strcmp(di->icons[XD_DESK_ICON_COMPUTER], "/i/pc.png") == 0);
    CHECK(di->icons[XD_DESK_ICON_TRASH_FULL][0] == '\0'); /* only absolute paths */
    CHECK(strcmp(xd_desk_icon_default(XD_DESK_ICON_TRASH_EMPTY), "user-trash") == 0);

    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && b.desktop_icons.auto_arrange &&
          strcmp(b.desktop_icons.icons[XD_DESK_ICON_COMPUTER], "/i/pc.png") == 0);
    xd_strbuf_free(&text);

    /* Desktop Items' Apply keeps an Auto Arrange the shell saved since. */
    xd_settings base;
    xd_settings_defaults(&base);
    xd_settings pending = base;
    pending.desktop_icons.network = false;
    xd_settings now = base;
    now.desktop_icons.auto_arrange = true;
    xd_settings_merge(&base, &pending, &now);
    CHECK(now.desktop_icons.auto_arrange && !now.desktop_icons.network);
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
    /* The battery column and a scheme of the user's own. */
    xd_settings_parse(XD_STR("[Power]\nMonitorOffOnBattery=5\nStandbyOnBattery=10\n"
                             "HibernateOnBattery=45\nCustomScheme=My Lab\n"),
                      &a);
    CHECK(a.monitor_off_battery == 5 && a.standby_battery == 10 && a.hibernate_battery == 45);
    CHECK(strcmp(a.power_scheme_custom, "My Lab") == 0 && a.standby == 0);
    xd_settings_serialize(&a, &text);
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && b.hibernate_battery == 45);
    CHECK(strcmp(b.power_scheme_custom, "My Lab") == 0);
    xd_strbuf_free(&text);
    /* Out of range or unknown: the defaults stay. */
    xd_settings_parse(XD_STR("[Power]\nScheme=turbo\nStandby=-5\nHibernate=99999\n"), &a);
    CHECK(a.power_scheme == XD_POWER_HOME && a.standby == 0 && a.hibernate == 0);
}

/* Power Options' Alarms: the keys, and when each alarm goes off. */
static void test_battery_alarms(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    /* XP's levels, sound and message on, no action. */
    CHECK(a.low_alarm.on && a.low_alarm.percent == 10 && a.low_alarm.sound &&
          a.low_alarm.message && a.low_alarm.action == XD_ALARM_NOTHING);
    CHECK(a.critical_alarm.on && a.critical_alarm.percent == 3 &&
          a.critical_alarm.action == XD_ALARM_NOTHING);
    xd_settings_parse(XD_STR("[Power]\nLowAlarmLevel=25\nLowAlarmSound=false\n"
                             "CriticalAlarmAction=hibernate\nCriticalAlarmMessage=false\n"
                             "LowAlarmAction=reboot\nCriticalAlarmLevel=101\n"),
                      &a);
    CHECK(a.low_alarm.percent == 25 && !a.low_alarm.sound && a.low_alarm.message);
    /* An unknown action and a level out of range: the defaults. */
    CHECK(a.low_alarm.action == XD_ALARM_NOTHING && a.critical_alarm.percent == 3);
    CHECK(a.critical_alarm.action == XD_ALARM_HIBERNATE && !a.critical_alarm.message);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "CriticalAlarmAction=hibernate") != nullptr);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && b.low_alarm.percent == 25);
    xd_strbuf_free(&text);

    /* Discharging from 30%: low at 25, critical at 3, each once. */
    xd_alarm_done done = {0};
    CHECK(xd_battery_alarm_check(&a, &done, 30, true) == XD_ALARM_FIRED_NONE);
    CHECK(xd_battery_alarm_check(&a, &done, 25, true) == XD_ALARM_FIRED_LOW);
    CHECK(xd_battery_alarm_check(&a, &done, 20, true) == XD_ALARM_FIRED_NONE);
    CHECK(xd_battery_alarm_check(&a, &done, 3, true) == XD_ALARM_FIRED_CRITICAL);
    CHECK(xd_battery_alarm_check(&a, &done, 1, true) == XD_ALARM_FIRED_NONE);
    /* Plugged in, then unplugged again lower down: they start over, and a
     * fall straight past both is only the critical one. */
    CHECK(xd_battery_alarm_check(&a, &done, 2, false) == XD_ALARM_FIRED_NONE);
    CHECK(xd_battery_alarm_check(&a, &done, 2, true) == XD_ALARM_FIRED_CRITICAL);
    CHECK(xd_battery_alarm_check(&a, &done, 2, true) == XD_ALARM_FIRED_NONE);
    /* An alarm turned off never goes off. */
    a.critical_alarm.on = false;
    done = (xd_alarm_done){0};
    CHECK(xd_battery_alarm_check(&a, &done, 2, true) == XD_ALARM_FIRED_LOW);
    CHECK(xd_battery_alarm_check(&a, &done, 1, true) == XD_ALARM_FIRED_NONE);
}

/* Accessibility Options' Sound tab, status indicators and reset. */
static void test_access_extra_settings(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    const xd_access_settings *ac = &a.access;
    CHECK(ac->sticky_indicator && ac->filter_indicator && ac->mouse_keys_indicator);
    CHECK(!ac->sound_sentry && ac->sound_sentry_effect == XD_SENTRY_WINDOW && !ac->show_sounds);
    CHECK(!ac->auto_reset && ac->auto_reset_minutes == XD_AUTO_RESET_MIN);
    xd_settings_parse(XD_STR("[Accessibility]\nSoundSentry=true\nSoundSentryEffect=desktop\n"
                             "ShowSounds=true\nMouseKeysIndicator=false\nAutoReset=true\n"
                             "AutoResetMinutes=20\n"),
                      &a);
    CHECK(ac->sound_sentry && ac->sound_sentry_effect == XD_SENTRY_DESKTOP && ac->show_sounds);
    CHECK(!ac->mouse_keys_indicator && ac->auto_reset && ac->auto_reset_minutes == 20);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && b.access.sound_sentry_effect == XD_SENTRY_DESKTOP);
    xd_strbuf_free(&text);
    /* An unknown effect and minutes out of range: the defaults. */
    xd_settings_parse(XD_STR("[Accessibility]\nSoundSentryEffect=strobe\nAutoResetMinutes=2\n"),
                      &a);
    CHECK(ac->sound_sentry_effect == XD_SENTRY_WINDOW && ac->auto_reset_minutes == 5);
}

static void test_power_schemes(void) {
    /* No file: XP's six, with both columns. */
    GPtrArray *s = xd_power_schemes_load("/nonexistent/power-schemes.ini");
    CHECK(s->len == XD_POWER_SCHEME_COUNT);
    xd_power_scheme_def *d = xd_power_schemes_find(s, "Portable/Laptop");
    CHECK(d != nullptr && d->builtin == XD_POWER_PORTABLE && d->ac.hibernate == 180 &&
          d->dc.monitor == 5);
    CHECK(strcmp(xd_power_builtin_label(XD_POWER_MAX_BATTERY), "Max Battery") == 0);
    CHECK(xd_power_scheme_name_valid("My Lab") && !xd_power_scheme_name_valid("") &&
          !xd_power_scheme_name_valid("a[b]") && !xd_power_scheme_name_valid(" pad") &&
          !xd_power_scheme_name_valid("tab\there"));

    /* A changed built-in one, a deleted one and one of the user's own
     * come back as saved. */
    d->ac.monitor = 3;
    g_ptr_array_remove(s, xd_power_schemes_find(s, "Max Battery"));
    d = xd_power_schemes_put(s, "My Lab", (xd_power_timers){10, 30, 0},
                             (xd_power_timers){2, 5, 60});
    CHECK(d->builtin == -1 && s->len == XD_POWER_SCHEME_COUNT);
    char *dir = g_dir_make_tmp("xd-power-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "xeondesktop", "power-schemes.ini", nullptr);
    CHECK(xd_power_schemes_save(path, s, nullptr));
    g_ptr_array_unref(s);
    s = xd_power_schemes_load(path);
    CHECK(s->len == XD_POWER_SCHEME_COUNT && xd_power_schemes_find(s, "Max Battery") == nullptr);
    d = xd_power_schemes_find(s, "Portable/Laptop");
    CHECK(d != nullptr && d->ac.monitor == 3 && d->ac.standby == 20);
    d = g_ptr_array_index(s, s->len - 1);
    CHECK(strcmp(d->name, "My Lab") == 0 && d->ac.standby == 30 && d->dc.hibernate == 60);
    /* Saving over a name replaces its timers; a deleted built-in one can
     * be saved again. */
    xd_power_schemes_put(s, "My Lab", (xd_power_timers){1, 2, 3}, (xd_power_timers){4, 5, 6});
    d = xd_power_schemes_put(s, "Max Battery", (xd_power_timers){0}, (xd_power_timers){0});
    CHECK(d->builtin == XD_POWER_MAX_BATTERY && s->len == XD_POWER_SCHEME_COUNT + 1);
    CHECK(xd_power_schemes_find(s, "My Lab")->dc.monitor == 4);
    g_ptr_array_unref(s);
    g_remove(path);
    char *sub = g_path_get_dirname(path);
    g_rmdir(sub);
    g_free(sub);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
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

    /* The accessibility tones: 16-bit mono WAVs, the RIFF sizes right,
     * quiet, and silent at both ends (no click). */
    for (int tone = 0; tone < XD_TONE_COUNT; tone++) {
        GBytes *wav = xd_sound_tone_wav((xd_sound_tone)tone);
        gsize len = 0;
        const uint8_t *d = g_bytes_get_data(wav, &len);
        CHECK(len > 44 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WAVEfmt ", 8) == 0);
        uint32_t riff = d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 | (uint32_t)d[7] << 24;
        uint32_t data = d[40] | (uint32_t)d[41] << 8 | (uint32_t)d[42] << 16 | (uint32_t)d[43] << 24;
        CHECK(riff == len - 8 && data == len - 44 && d[22] == 1 && d[34] == 16);
        int peak = 0;
        for (gsize i = 44; i + 1 < len; i += 2)
            peak = MAX(peak, abs((int16_t)(d[i] | d[i + 1] << 8)));
        CHECK(peak > 1000 && peak <= INT16_MAX / 3 + 1);
        CHECK(d[44] == 0 && d[45] == 0);
        CHECK(xd_sound_tone_name((xd_sound_tone)tone)[0] != '\0');
        g_bytes_unref(wav);
    }
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
    static const char *const schemes[] = {"",           "windows-classic", "high-contrast-black",
                                          "high-contrast-white", "luna-olive", "luna-silver",
                                          "luna-energyblue"};
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

static void check_text(char *s, const char *want) {
    if (strcmp(s, want) != 0)
        g_printerr("  got \"%s\", want \"%s\"\n", s, want);
    CHECK(strcmp(s, want) == 0);
    g_free(s);
}

/* Regional and Language Options' Customize. */
static void test_intl_custom(void) {
    /* XP's pictures and strftime, both ways. */
    check_text(xd_intl_picture_to_strftime("h:mm:ss tt"), "%-I:%M:%S %p");
    check_text(xd_intl_picture_to_strftime("HH'h'mm"), "%Hh%M");
    check_text(xd_intl_picture_to_strftime("dddd, MMMM dd, yyyy"), "%A, %B %d, %Y");
    check_text(xd_intl_picture_to_strftime("d.M.yy '100%'"), "%-d.%-m.%y 100%%");
    check_text(xd_intl_strftime_to_picture("%r", "%I:%M:%S %p"), "hh:mm:ss tt");
    check_text(xd_intl_strftime_to_picture("%H:%M:%S", nullptr), "HH:mm:ss");
    check_text(xd_intl_strftime_to_picture("%m/%d/%Y", nullptr), "MM/dd/yyyy");
    check_text(xd_intl_strftime_to_picture("%-d. %B %Y", nullptr), "d. MMMM yyyy");
    check_text(xd_intl_strftime_to_picture("%H時%M分", nullptr), "HH時mm分");
    check_text(xd_intl_strftime_to_picture("%Hh%M %Z", nullptr), "HH'h'mm ");
    check_text(xd_intl_strftime_to_picture("%F", nullptr), "yyyy-MM-dd");
    check_text(xd_intl_picture_separator("h:mm:ss tt"), ":");
    check_text(xd_intl_picture_separator("dd.MM.yyyy"), ".");
    check_text(xd_intl_picture_separator("HH"), "");
    check_text(xd_intl_picture_replace_separator("M/d/yyyy", "/", "-"), "M-d-yyyy");
    check_text(xd_intl_picture_replace_separator("H:mm 'a:b'", ":", "h"), "H'h'mm 'a:b'");

    /* Digit grouping as localedef writes it. */
    check_text(xd_intl_group_digits("123456789", "3;3", ","), "123,456,789");
    check_text(xd_intl_group_digits("123456789", "3;2", ","), "12,34,56,789");
    check_text(xd_intl_group_digits("123456789", "-1", ","), "123456789");
    check_text(xd_intl_group_digits("12", "3", " "), "12");
    check_text(xd_intl_grouping_text("\3\3"), "3;3");
    check_text(xd_intl_grouping_text(""), "-1");
    check_text(xd_intl_grouping_text("\3\177"), "3;-1");

    /* Samples from the effective settings. */
    xd_intl_custom e;
    xd_intl_custom_init(&e);
    e.decimal = g_strdup(",");
    e.thousands = g_strdup(".");
    e.grouping = g_strdup("3;3");
    e.currency = g_strdup("€");
    e.mon_decimal = g_strdup(",");
    e.mon_thousands = g_strdup(".");
    e.mon_grouping = g_strdup("3");
    e.frac_digits = 2;
    e.positive_format = 3;
    e.negative_format = 8;
    check_text(xd_intl_number_sample(&e, false), "123.456.789,00");
    check_text(xd_intl_number_sample(&e, true), "-123.456.789,00");
    check_text(xd_intl_currency_sample(&e, false), "123.456.789,00 €");
    check_text(xd_intl_currency_sample(&e, true), "-123.456.789,00 €");
    e.negative_format = 0;
    e.frac_digits = 0;
    check_text(xd_intl_currency_sample(&e, true), "(€123.456.789)");
    e.time_format = g_strdup("HH.mm");
    e.am = g_strdup("vorm.");
    e.pm = g_strdup("nachm.");
    e.date_format = g_strdup("dd/MM/yyyy");
    /* 2001-02-03 04:05:06 local time */
    struct tm tm = {.tm_year = 101, .tm_mon = 1, .tm_mday = 3, .tm_hour = 4, .tm_min = 5,
                    .tm_sec = 6, .tm_isdst = -1};
    time_t when = mktime(&tm);
    check_text(xd_intl_time_sample("C.UTF-8", &e, when), "04.05");
    g_free(e.time_format);
    e.time_format = g_strdup("h:mm tt");
    check_text(xd_intl_time_sample("C.UTF-8", &e, when), "4:05 vorm.");
    check_text(xd_intl_date_sample("C.UTF-8", &e, when, false), "03/02/2001");

    /* formats.ini keeps only what was changed. */
    char *dir = g_dir_make_tmp("xd-custom-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "formats.ini", nullptr);
    xd_intl_custom c;
    xd_intl_custom_init(&c);
    CHECK(xd_intl_custom_empty(&c));
    CHECK(xd_intl_custom_save(path, "C.UTF-8", &c, nullptr) &&
          !g_file_test(path, G_FILE_TEST_EXISTS));
    c.thousands = g_strdup(" ");
    c.grouping = g_strdup("3;2");
    c.negative_format = 12;
    c.time_format = g_strdup("HH:mm");
    CHECK(!xd_intl_custom_empty(&c));
    CHECK(xd_intl_custom_save(path, "C.UTF-8", &c, nullptr));
    xd_intl_custom back;
    char *locale = nullptr;
    xd_intl_custom_load(path, &locale, &back);
    CHECK(locale != nullptr && strcmp(locale, "C.UTF-8") == 0);
    CHECK(back.thousands != nullptr && strcmp(back.thousands, " ") == 0);
    CHECK(back.grouping != nullptr && strcmp(back.grouping, "3;2") == 0);
    CHECK(back.negative_format == 12 && back.positive_format == -1 && back.decimal == nullptr);
    CHECK(back.time_format != nullptr && strcmp(back.time_format, "HH:mm") == 0);
    g_free(locale);
    xd_intl_custom_clear(&back);
    g_file_set_contents(path, "[Customize]\nLocale=x;y\nDigitGrouping=3;a\nCurrencyDigits=42\n",
                        -1, nullptr);
    xd_intl_custom_load(path, &locale, &back);
    CHECK(locale == nullptr && xd_intl_custom_empty(&back));

    check_text(xd_intl_custom_name("de_DE.UTF-8"), "de_DE.UTF-8@xeondesktop");
    check_text(xd_intl_custom_base("de_DE.UTF-8@xeondesktop"), "de_DE.UTF-8");
    check_text(xd_intl_custom_base("de_DE.UTF-8"), "de_DE.UTF-8");
    char *src = nullptr, *map = nullptr;
    xd_intl_source_of("sr_RS.UTF-8@latin", &src, &map);
    CHECK(strcmp(src, "sr_RS@latin") == 0 && strcmp(map, "UTF-8") == 0);
    g_free(src);
    g_free(map);
    xd_intl_source_of("de_DE.utf8", &src, &map);
    CHECK(strcmp(src, "de_DE") == 0 && strcmp(map, "UTF-8") == 0);
    g_free(src);
    g_free(map);

    /* The formats file with LOCPATH. */
    CHECK(xd_intl_write_formats_full(path, "C.UTF-8@xeondesktop", "/l/p", nullptr));
    char *text = nullptr;
    CHECK(g_file_get_contents(path, &text, nullptr, nullptr) &&
          g_str_has_prefix(strchr(text, '\n') + 1, "LOCPATH=/l/p\n") &&
          strstr(text, "\nLC_TIME=C.UTF-8@xeondesktop\n") != nullptr);
    g_free(text);
    char *read = xd_intl_read_formats(path);
    CHECK(read != nullptr && strcmp(read, "C.UTF-8@xeondesktop") == 0);
    g_free(read);
    g_remove(path);

    /* The locale compiled for real, where localedef and en_US's source
     * are there: glibc then formats with the changes. */
    locale_t en = newlocale(LC_ALL_MASK, "en_US.UTF-8", (locale_t)0);
    char *localedef = g_find_program_in_path("localedef");
    if (en == (locale_t)0 || localedef == nullptr ||
        !g_file_test("/usr/share/i18n/locales/en_US", G_FILE_TEST_EXISTS)) {
        g_printerr("test_intl_custom: no en_US.UTF-8 or localedef, skipped compiling\n");
    } else {
        xd_intl_custom from;
        CHECK(xd_intl_custom_from_locale("en_US.UTF-8", &from));
        CHECK(strcmp(from.decimal, ".") == 0 && strcmp(from.grouping, "3") == 0 &&
              from.measurement == 2 && strcmp(from.time_format, "hh:mm:ss tt") == 0 &&
              g_str_has_prefix(from.date_format, "MM/dd/yy") && from.negative_format == 1);
        xd_intl_custom_clear(&from);
        char *source = xd_intl_custom_source("en_US.UTF-8", &c);
        CHECK(source != nullptr && strstr(source, "LC_NUMERIC\ndecimal_point") != nullptr &&
              strstr(source, "grouping 3;2\n") != nullptr &&
              strstr(source, "LC_PAPER\ncopy") != nullptr);
        char *srcfile = g_build_filename(dir, "custom.src", nullptr);
        g_file_set_contents(srcfile, source, -1, nullptr);
        char *out = g_build_filename(dir, "en_US.UTF-8@xeondesktop", nullptr);
        const char *argv[] = {localedef, "--no-archive", "-i", srcfile, "-f", "UTF-8", out,
                              nullptr};
        int status = -1;
        CHECK(g_spawn_sync(nullptr, (char **)(uintptr_t)argv, nullptr,
                           (GSpawnFlags)(G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                           nullptr, nullptr, nullptr, nullptr, &status, nullptr));
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) <= 1);
        g_setenv("LOCPATH", dir, TRUE);
        locale_t mine = newlocale(LC_NUMERIC_MASK | LC_MONETARY_MASK | LC_TIME_MASK,
                                  "en_US.UTF-8@xeondesktop", (locale_t)0);
        g_unsetenv("LOCPATH");
        CHECK(mine != (locale_t)0);
        if (mine != (locale_t)0) {
            CHECK(strcmp(nl_langinfo_l(THOUSEP, mine), " ") == 0);
            CHECK(strcmp(nl_langinfo_l(T_FMT, mine), "%H:%M") == 0);
            CHECK(strcmp(nl_langinfo_l(MON_1, mine), "January") == 0);
            char buf[64];
            CHECK(strfmon_l(buf, sizeof(buf), mine, "%n", -1234.5) > 0 &&
                  strcmp(buf, "$ -1,234.50") == 0);
            freelocale(mine);
        }
        const char *rm[] = {"rm", "-rf", out, nullptr};
        g_spawn_sync(nullptr, (char **)(uintptr_t)rm, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr);
        g_remove(srcfile);
        g_free(srcfile);
        g_free(out);
        g_free(source);
    }
    if (en != (locale_t)0)
        freelocale(en);
    g_free(localedef);
    xd_intl_custom_clear(&c);
    xd_intl_custom_clear(&e);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
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

    /* xd-files' Details columns, as each folder remembers them. */
    int order[8] = {0}, width[8] = {0}, count = 0;
    CHECK(xd_columns_parse("0:250,3:0,1:80,6:70", 8, order, &count, width));
    CHECK(count == 4 && order[0] == 0 && order[1] == 3 && order[2] == 1 && order[3] == 6);
    CHECK(width[0] == 250 && width[3] == 0 && width[1] == 80 && width[6] == 70 && width[2] == 0);
    char *spec = xd_columns_format(order, count, width);
    CHECK(strcmp(spec, "0:250,3:0,1:80,6:70") == 0);
    g_free(spec);
    CHECK(xd_columns_parse("2:100,0:0", 8, order, &count, width)); /* Name moved right */
    CHECK(count == 2 && order[0] == 2 && order[1] == 0);
    CHECK(xd_columns_parse("1:80", 8, order, &count, width)); /* Name always shown */
    CHECK(count == 2 && order[0] == 0 && order[1] == 1);
    CHECK(!xd_columns_parse("0:10,0:20", 8, order, &count, width));
    CHECK(!xd_columns_parse("9:10", 8, order, &count, width));
    CHECK(!xd_columns_parse("0:x", 8, order, &count, width));
    CHECK(!xd_columns_parse("0", 8, order, &count, width));
    CHECK(!xd_columns_parse("", 8, order, &count, width));
    CHECK(!xd_columns_parse(nullptr, 8, order, &count, width));
    CHECK(count == 2 && order[1] == 1); /* left alone when refused */

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
    /* A database that deletes the globs below it says so. */
    exts = g_ptr_array_new_with_free_func(g_free);
    CHECK(xd_mime_xml_extensions("<mime-type type=\"a/b\"><glob pattern=\"*.x\"/><glob-deleteall/>"
                                 "<glob pattern=\"*.y\"/></mime-type>",
                                 -1, exts));
    CHECK(exts->len == 1 && strcmp(g_ptr_array_index(exts, 0), "y") == 0);
    g_ptr_array_unref(exts);
}

static bool has_glob(GPtrArray *globs, const char *pattern) {
    for (guint i = 0; i < globs->len; i++)
        if (strcmp(((xd_mime_glob *)g_ptr_array_index(globs, i))->pattern, pattern) == 0)
            return true;
    return false;
}

/* File Types' New and Delete, on databases in a scratch directory. */
static void test_file_types_edit(void) {
    xd_mime_package pkg;
    const char *xml = "<?xml version=\"1.0\"?><mime-info xmlns=\"x\">"
                      "<mime-type type=\"text/other\"><glob pattern=\"*.no\"/></mime-type>"
                      "<mime-type type=\"text/x-test\"><comment>Test &amp; file</comment>"
                      "<comment xml:lang=\"de\">Testdatei</comment>"
                      "<glob pattern=\"*.old\"/><glob-deleteall/>"
                      "<glob pattern=\"*.TST\" weight=\"60\" case-sensitive=\"true\"/>"
                      "<sub-class-of type=\"text/plain\"><glob pattern=\"*.deep\"/></sub-class-of>"
                      "</mime-type></mime-info>";
    CHECK(xd_mime_package_parse(xml, -1, "text/x-test", &pkg));
    CHECK(strcmp(pkg.comment, "Test & file") == 0 && pkg.delete_all && pkg.globs->len == 1);
    const xd_mime_glob *g = g_ptr_array_index(pkg.globs, 0);
    CHECK(strcmp(g->pattern, "*.TST") == 0 && g->weight == 60 && g->case_sensitive);
    char *out = xd_mime_package_to_xml(&pkg);
    xd_mime_package_clear(&pkg);
    CHECK(xd_mime_package_parse(out, -1, "text/x-test", &pkg));
    CHECK(strcmp(pkg.comment, "Test & file") == 0 && pkg.delete_all && pkg.globs->len == 1);
    xd_mime_package_clear(&pkg);
    g_free(out);
    CHECK(!xd_mime_package_parse(xml, -1, "text/absent", &pkg));
    xd_mime_package_clear(&pkg);

    CHECK(xd_mime_extension_valid("xyz") && xd_mime_extension_valid("c++") &&
          !xd_mime_extension_valid("") && !xd_mime_extension_valid("a.b") &&
          !xd_mime_extension_valid("*") && !xd_mime_extension_valid("a b") &&
          !xd_mime_extension_valid("../x"));
    char *type = xd_mime_type_for_new_extension("XYZ");
    CHECK(strcmp(type, "application/x-extension-xyz") == 0);

    char *update = g_find_program_in_path("update-mime-database");
    if (update == nullptr) {
        g_printerr("test_file_types_edit: no update-mime-database, skipped the databases\n");
        g_free(type);
        return;
    }
    g_free(update);
    char *root = g_dir_make_tmp("xd-mime-XXXXXX", nullptr);
    char *user = g_build_filename(root, "user", nullptr);
    char *sys = g_build_filename(root, "sys", nullptr);
    const char *dirs[] = {user, sys, nullptr};
    char *pkgdir = g_build_filename(sys, "mime", "packages", nullptr);
    g_mkdir_with_parents(pkgdir, 0700);
    char *sysfile = g_build_filename(pkgdir, "sys.xml", nullptr);
    g_file_set_contents(sysfile,
                        "<?xml version=\"1.0\"?><mime-info xmlns=\"http://www.freedesktop.org/"
                        "standards/shared-mime-info\"><mime-type type=\"text/x-test\">"
                        "<comment>Test file</comment><glob pattern=\"*.tst\"/>"
                        "<glob pattern=\"*.tes\"/><glob pattern=\"TEST*\"/></mime-type>"
                        "</mime-info>",
                        -1, nullptr);
    CHECK(xd_mime_update_database(sys, nullptr));

    /* New, to an existing type: added to what the system has. */
    CHECK(xd_mime_add_extension(dirs, "text/x-test", "abc", nullptr, nullptr));
    CHECK(xd_mime_update_database(user, nullptr));
    GPtrArray *globs = xd_mime_type_globs(dirs, "text/x-test");
    CHECK(globs->len == 4 && has_glob(globs, "*.abc") && has_glob(globs, "*.tst"));
    g_ptr_array_unref(globs);
    /* Delete, of the system's extension: hidden, the others kept. */
    bool gone = true;
    CHECK(xd_mime_remove_extension(dirs, "text/x-test", "TST", &gone, nullptr) && !gone);
    CHECK(xd_mime_update_database(user, nullptr));
    globs = xd_mime_type_globs(dirs, "text/x-test");
    CHECK(globs->len == 3 && !has_glob(globs, "*.tst") && has_glob(globs, "*.abc") &&
          has_glob(globs, "*.tes") && has_glob(globs, "TEST*"));
    g_ptr_array_unref(globs);
    /* New, to a type of its own; Delete takes the type too. */
    CHECK(xd_mime_add_extension(dirs, type, "xyz", "XYZ File", nullptr));
    CHECK(xd_mime_update_database(user, nullptr));
    char *txml = g_build_filename(user, "mime", "application", "x-extension-xyz.xml", nullptr);
    char *text = nullptr;
    CHECK(g_file_get_contents(txml, &text, nullptr, nullptr) &&
          strstr(text, "XYZ File") != nullptr);
    g_free(text);
    CHECK(xd_mime_remove_extension(dirs, type, "xyz", &gone, nullptr) && gone);
    CHECK(xd_mime_update_database(user, nullptr));
    CHECK(!g_file_test(txml, G_FILE_TEST_EXISTS));
    g_free(txml);
    CHECK(!xd_mime_add_extension(dirs, "../evil", "x", nullptr, nullptr));

    /* Scratch databases away. */
    const char *cmd[] = {"rm", "-rf", root, nullptr};
    g_spawn_sync(nullptr, (char **)(uintptr_t)cmd, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr);
    g_free(sysfile);
    g_free(pkgdir);
    g_free(user);
    g_free(sys);
    g_free(root);
    g_free(type);
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
    CHECK(a.access.sticky_keys && !a.access.sticky_lock && a.access.filter_keys &&
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

    /* XP's defaults: off, but every shortcut on, with its warning. */
    xd_settings_defaults(&a);
    CHECK(!a.access.filter_keys && !a.access.toggle_keys && !a.access.mouse_keys);
    CHECK(a.access.sticky_shortcut && a.access.filter_shortcut && a.access.toggle_shortcut);
    CHECK(a.access.mouse_keys_shortcut && a.access.warn && a.access.sound_on_off);
    CHECK(a.access.slow_keys && a.access.slow_ms == 1000 && !a.mouse.click_lock);
    CHECK(a.mouse.click_lock_ms == 1200 && a.access.mouse_keys_numlock);
    xd_settings_parse(XD_STR("[Mouse]\nClickLock=true\nClickLockMs=800\n[Accessibility]\n"
                             "FilterKeys=true\nSlowKeys=false\nSlowKeysMs=300\n"
                             "FilterKeysNoRepeat=true\nToggleKeys=true\nToggleKeysShortcut=0\n"
                             "MouseKeys=true\nMouseKeysSpeed=200\nMouseKeysAccelMs=1500\n"
                             "MouseKeysNumLock=false\nWarnOnShortcut=false\n"
                             "StickyKeysTwoKeysOff=false\n"),
                      &a);
    CHECK(a.mouse.click_lock && a.mouse.click_lock_ms == 800);
    CHECK(a.access.filter_keys && !a.access.slow_keys && a.access.slow_ms == 300);
    CHECK(a.access.filter_no_repeat && a.access.toggle_keys && !a.access.toggle_shortcut);
    CHECK(a.access.mouse_keys && a.access.mouse_keys_speed == 200);
    CHECK(a.access.mouse_keys_accel_ms == 1500 && !a.access.mouse_keys_numlock);
    CHECK(!a.access.warn && !a.access.sticky_two_keys_off && a.access.sticky_sound);
    xd_settings_serialize(&a, &text);
    xd_settings_parse(xd_strbuf_view(&text), &b);
    xd_strbuf_free(&text);
    CHECK(memcmp(&a.access, &b.access, sizeof(a.access)) == 0);
    CHECK(memcmp(&a.mouse, &b.mouse, sizeof(a.mouse)) == 0);
    /* Out of range keeps the default. */
    xd_settings_parse(XD_STR("[Mouse]\nClickLockMs=50\n[Accessibility]\nMouseKeysSpeed=9000\n"),
                      &a);
    CHECK(a.mouse.click_lock_ms == 1200 && a.access.mouse_keys_speed == 80);

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

/* Restores an environment variable a test changed. */
static void env_restore(const char *var, char *saved) {
    if (saved != nullptr)
        g_setenv(var, saved, TRUE);
    else
        g_unsetenv(var);
    g_free(saved);
}

static void test_qt_style(void) {
    /* [Appearance] QtStyle: a style key, "" = automatic. */
    xd_settings a;
    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nQtStyle=kvantum-dark\n"), &a);
    CHECK(strcmp(a.qt_style, "kvantum-dark") == 0);
    CHECK(strcmp(xd_settings_qt_style(&a), "kvantum-dark") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "QtStyle=kvantum-dark") != nullptr);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(xd_settings_equal(&a, &b) && strcmp(b.qt_style, "kvantum-dark") == 0);
    xd_strbuf_free(&text);
    /* It goes into the environment: no spaces, paths or shell syntax. */
    static const char *const bad[] = {
        "[Appearance]\nQtStyle=../breeze\n", "[Appearance]\nQtStyle=Br eeze\n",
        "[Appearance]\nQtStyle=a;b\n",
        "[Appearance]\nQtStyle=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"};
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        xd_settings_parse(xd_str_from_cstr(bad[i]), &a);
        CHECK(a.qt_style[0] == '\0');
    }
    /* Automatic: 2000's bevels for Classic, Fusion for the rest. */
    xd_settings_defaults(&a);
    CHECK(a.qt_style[0] == '\0');
    CHECK(strcmp(xd_settings_qt_style(&a), "Windows") == 0);
    static const xd_visual_style fusion[] = {XD_STYLE_LUNA, XD_STYLE_WHISTLER, XD_STYLE_AERO};
    for (size_t i = 0; i < G_N_ELEMENTS(fusion); i++) {
        a.style = fusion[i];
        CHECK(strcmp(xd_settings_qt_style(&a), "Fusion") == 0);
    }
    CHECK(strcmp(xd_qt_style_auto(XD_STYLE_CLASSIC), "Windows") == 0);

    /* The environment: ours follows the setting, the user's stays. */
    static const char *const vars[] = {"QT_QPA_PLATFORMTHEME",
                                       "QT_STYLE_OVERRIDE",
                                       "QT_GUI_GTK_JSON",
                                       "XEONDESKTOP_QT_QPA_PLATFORMTHEME",
                                       "XEONDESKTOP_QT_STYLE_OVERRIDE",
                                       "XEONDESKTOP_QT_GUI_GTK_JSON"};
    char *saved[G_N_ELEMENTS(vars)];
    for (size_t i = 0; i < G_N_ELEMENTS(vars); i++) {
        saved[i] = g_strdup(g_getenv(vars[i]));
        g_unsetenv(vars[i]);
    }
    xd_settings_defaults(&a);
    CHECK(xd_settings_qt_env(&a));
    CHECK(g_strcmp0(g_getenv("QT_QPA_PLATFORMTHEME"), "gtk3") == 0);
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "Windows") == 0);
    CHECK(xd_qt_env_ours("QT_QPA_PLATFORMTHEME") && xd_qt_env_ours("QT_STYLE_OVERRIDE"));
    /* Our palette with our GTK theme, none with another. */
    CHECK(g_str_has_suffix(g_getenv("QT_GUI_GTK_JSON"), "/XeonDesktop/" XD_QT_PALETTE_FILE));
    CHECK(!xd_settings_qt_env(&a)); /* nothing new */
    snprintf(a.gtk_theme, sizeof(a.gtk_theme), "%s", "Adwaita");
    CHECK(xd_settings_qt_env(&a));
    CHECK(g_strcmp0(g_getenv("QT_GUI_GTK_JSON"), "") == 0 && xd_qt_env_ours("QT_GUI_GTK_JSON"));
    a.gtk_theme[0] = '\0';
    a.style = XD_STYLE_AERO; /* a child of ours, after a change */
    CHECK(xd_settings_qt_env(&a));
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "Fusion") == 0);
    /* The user's own (qt6ct, Kvantum), set before the desktop started... */
    for (size_t i = 0; i < G_N_ELEMENTS(vars); i++)
        g_unsetenv(vars[i]);
    g_setenv("QT_QPA_PLATFORMTHEME", "qt6ct", TRUE);
    g_setenv("QT_STYLE_OVERRIDE", "kvantum", TRUE);
    xd_settings_qt_env(&a); /* only adds our palette file, which they ignore */
    CHECK(!xd_settings_qt_env(&a));
    CHECK(g_strcmp0(g_getenv("QT_QPA_PLATFORMTHEME"), "qt6ct") == 0);
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "kvantum") == 0);
    CHECK(!xd_qt_env_ours("QT_STYLE_OVERRIDE"));
    /* ...or over ours, for one program started from a terminal. */
    g_unsetenv("QT_STYLE_OVERRIDE");
    CHECK(xd_settings_qt_env(&a) && xd_qt_env_ours("QT_STYLE_OVERRIDE"));
    g_setenv("QT_STYLE_OVERRIDE", "Breeze", TRUE);
    CHECK(!xd_settings_qt_env(&a));
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "Breeze") == 0);
    for (size_t i = 0; i < G_N_ELEMENTS(vars); i++)
        env_restore(vars[i], saved[i]);

    /* Styles: Qt's built-in two first, then plugins by key, once each
     * (a Qt 5 and a Qt 6 build of one, and one named like a built-in). */
    char *root = g_dir_make_tmp("xd-qtstyles-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    put_file(root, "qt6/styles/breeze6.so", "");
    put_file(root, "qt6/styles/libkvantum.so", "");
    put_file(root, "qt6/styles/libqt6ct-style.so", "");
    put_file(root, "qt6/styles/README", "");
    put_file(root, "qt5/styles/breeze.so", "");
    put_file(root, "qt5/styles/libfusion.so", "");
    put_file(root, "qt5/styles/oxygen.so", "");
    char *qt6 = g_build_filename(root, "qt6", nullptr);
    char *qt5 = g_build_filename(root, "qt5", nullptr);
    const char *dirs[] = {qt6, qt5, "/nonexistent", nullptr};
    GPtrArray *styles = xd_qt_styles_list(dirs);
    static const char *const want[] = {"Windows", "Fusion", "Breeze", "Kvantum", "Oxygen",
                                       "Qt6ct-style"};
    CHECK(styles->len == G_N_ELEMENTS(want));
    for (guint i = 0; i < G_N_ELEMENTS(want); i++)
        CHECK(theme_at(styles, i) && strcmp(theme_at(styles, i)->dir, want[i]) == 0 &&
              strcmp(theme_at(styles, i)->name, want[i]) == 0);
    g_ptr_array_unref(styles);
    styles = xd_qt_styles_list(nullptr);
    CHECK(styles->len == 2);
    g_ptr_array_unref(styles);
    g_free(qt6);
    g_free(qt5);
    rm_tree(root);
    g_free(root);
    /* Only directories that exist, each once. */
    char **found = xd_qt_plugin_dirs();
    for (char **d = found; *d != nullptr; d++) {
        CHECK(g_file_test(*d, G_FILE_TEST_IS_DIR));
        for (char **e = d + 1; *e != nullptr; e++)
            CHECK(strcmp(*d, *e) != 0);
    }
    g_strfreev(found);
}

/* The colour Qt's palette file gives `role` in its active group. */
static char *qt_palette_color(JsonNode *root, const char *palette, const char *role) {
    JsonObject *o = json_object_get_object_member(json_node_get_object(root), "QtGtk3Palettes");
    o = o != nullptr ? json_object_get_object_member(o, palette) : nullptr;
    JsonArray *entries = o != nullptr ? json_object_get_array_member(o, role) : nullptr;
    for (guint i = 0; entries != nullptr && i < json_array_get_length(entries); i++) {
        JsonObject *e = json_array_get_object_element(entries, i);
        if (strcmp(json_object_get_string_member(e, "ColorGroup"), "Active") != 0 ||
            strcmp(json_object_get_string_member(e, "SourceType"), "Fixed") != 0)
            continue;
        JsonObject *data = json_object_get_object_member(e, "SourceData");
        JsonObject *brush = json_object_get_object_member(data, "FixedBrush");
        return g_strdup(json_object_get_string_member(brush, "Color"));
    }
    return nullptr;
}

static void test_qt_palette(void) {
    /* Windows Standard: white fields, the scheme's bevel colours (Qt's
     * own mapping makes fields grey and works bevels out from the face). */
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    xd_theme t;
    xd_theme_init(&t, XD_STYLE_CLASSIC, &scheme, 1.0);
    xd_strbuf json = {0};
    xd_qt_palette_json(&t, &json);
    JsonParser *parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, xd_strbuf_cstr(&json), (gssize)json.len, nullptr));
    JsonNode *root = json_parser_get_root(parser);
    static const struct {
        const char *palette, *role, *color;
    } want[] = {
        {"SystemPalette", "Window", "#d4d0c8"}, {"SystemPalette", "Base", "#ffffff"},
        {"SystemPalette", "Light", "#ffffff"},  {"SystemPalette", "Dark", "#808080"},
        {"SystemPalette", "Shadow", "#404040"}, {"SystemPalette", "Highlight", "#0a246a"},
        {"MenuPalette", "Window", "#d4d0c8"},
    };
    for (size_t i = 0; root != nullptr && i < G_N_ELEMENTS(want); i++) {
        char *c = qt_palette_color(root, want[i].palette, want[i].role);
        CHECK(g_strcmp0(c, want[i].color) == 0);
        g_free(c);
    }
    g_object_unref(parser);
    /* Luna: XP's white menus and its selection blue. */
    xd_theme_init(&t, XD_STYLE_LUNA, &scheme, 1.0);
    xd_qt_palette_json(&t, &json);
    parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, xd_strbuf_cstr(&json), (gssize)json.len, nullptr));
    root = json_parser_get_root(parser);
    char *c = root != nullptr ? qt_palette_color(root, "MenuPalette", "Window") : nullptr;
    CHECK(g_strcmp0(c, "#ffffff") == 0);
    g_free(c);
    c = root != nullptr ? qt_palette_color(root, "SystemPalette", "Highlight") : nullptr;
    CHECK(g_strcmp0(c, "#316ac5") == 0);
    g_free(c);
    g_object_unref(parser);
    xd_strbuf_free(&json);
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

    /* When the shell last started each program: the newest line wins. */
    char *times[] = {(char *)(uintptr_t)"1790000000\tfirefox.desktop",
                     (char *)(uintptr_t)"1780000000\tfirefox.desktop",
                     (char *)(uintptr_t)"1700000000\tgimp.desktop",
                     (char *)(uintptr_t)"0\tzero.desktop", (char *)(uintptr_t)"soon\tbad.desktop"};
    GHashTable *launched = xd_appwiz_parse_launch_times(times, G_N_ELEMENTS(times));
    CHECK(g_hash_table_size(launched) == 2);
    CHECK(xd_appwiz_launch_time(launched, "firefox.desktop") == 1790000000);
    CHECK(xd_appwiz_launch_time(launched, "gimp.desktop") == 1700000000);
    CHECK(xd_appwiz_launch_time(launched, "zero.desktop") == 0);
    CHECK(xd_appwiz_launch_time(nullptr, "gimp.desktop") == 0);
    g_hash_table_unref(launched);

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
    xd_appwiz_key a = {"Alpha", 100, 1, 50, "pacman (extra)"},
                  b = {"beta", 300, 1, 0, "Flatpak (flathub, system)"},
                  c = {"Gamma", 0, 9, 70, nullptr}, d = {"delta", 100, 0, 50, "pacman (extra)"};
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
    CHECK(xd_appwiz_compare(&b, &a, XD_SORT_SOURCE) < 0); /* by source */
    CHECK(xd_appwiz_compare(&a, &d, XD_SORT_SOURCE) < 0); /* then by name */
    CHECK(xd_appwiz_compare(&d, &c, XD_SORT_SOURCE) < 0); /* not known last */

    /* Where a program came from. */
    CHECK(strcmp(xd_appwiz_package_manager("alpm"), "pacman") == 0);
    CHECK(strcmp(xd_appwiz_package_manager("aptcc"), "APT") == 0);
    CHECK(strcmp(xd_appwiz_package_manager("mock"), "mock") == 0);
    CHECK(strcmp(xd_appwiz_package_manager(nullptr), "a package manager") == 0);
    static const char *const sources[][3] = {
        {"alpm", "gimp;3.0.4-1;x86_64;installed:extra", "pacman (extra)"},
        {"alpm", "alacritty;0.17.0-1;x86_64;installed", "pacman"},
        {"alpm", "mytool;1.0-1;x86_64;installed:local", "pacman (local package)"},
        {"aptcc", "vim;2:9.1;amd64;installed:debian-stable-main", "APT (debian-stable-main)"},
        {"dnf", "nano;8.0;x86_64;local", "DNF (local package)"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(sources); i++) {
        char *src = xd_appwiz_package_source(sources[i][0], sources[i][1]);
        CHECK(strcmp(src, sources[i][2]) == 0);
        g_free(src);
    }
    char *fp = xd_appwiz_flatpak_source("flathub", "default");
    CHECK(strcmp(fp, "Flatpak (flathub, system)") == 0);
    g_free(fp);
    fp = xd_appwiz_flatpak_source("fedora", "user");
    CHECK(strcmp(fp, "Flatpak (fedora, user)") == 0);
    g_free(fp);
    CHECK(strcmp(xd_appwiz_other_source("/home/me/Apps/Krita-5.2.AppImage"), "AppImage") == 0);
    CHECK(strcmp(xd_appwiz_other_source("/opt/tool/run.appimage"), "AppImage") == 0);
    CHECK(strcmp(xd_appwiz_other_source("/home/me/bin/tool"), "Not from a package") == 0);
    CHECK(strcmp(xd_appwiz_other_source(nullptr), "Not from a package") == 0);

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

/* Windows 7's taskbar: its buttons setting, jump lists' recent files for
 * one program, and moving pinned programs. */
static void test_superbar(void) {
    xd_settings a;
    xd_settings_defaults(&a);
    CHECK(a.buttons == XD_BUTTONS_ALWAYS_COMBINE); /* Windows 7's default */
    xd_settings_parse(XD_STR("[Taskbar]\nButtons=whenfull\n"), &a);
    CHECK(a.buttons == XD_BUTTONS_WHEN_FULL);
    xd_strbuf text = {0};
    xd_settings_serialize(&a, &text);
    xd_settings b;
    xd_settings_parse(xd_strbuf_view(&text), &b);
    CHECK(b.buttons == XD_BUTTONS_WHEN_FULL && xd_settings_equal(&a, &b));
    xd_strbuf_free(&text);
    xd_settings_parse(XD_STR("[Taskbar]\nButtons=sometimes\n"), &a);
    CHECK(a.buttons == XD_BUTTONS_ALWAYS_COMBINE);

    /* Recent files: the program's own, still there, the latest first. */
    char *dir = g_dir_make_tmp("xd-jump-XXXXXX", nullptr);
    CHECK(dir != nullptr);
    char *a_txt = g_build_filename(dir, "a.txt", nullptr);
    char *b_txt = g_build_filename(dir, "b.txt", nullptr);
    char *gone = g_build_filename(dir, "gone.txt", nullptr);
    char *c_png = g_build_filename(dir, "c.png", nullptr);
    CHECK(g_file_set_contents(a_txt, "a", 1, nullptr) && g_file_set_contents(b_txt, "b", 1, nullptr) &&
          g_file_set_contents(c_png, "c", 1, nullptr));
    GBookmarkFile *recent = g_bookmark_file_new();
    struct {
        const char *path, *app, *exec;
        int64_t when;
    } uses[] = {
        {a_txt, "org.gnome.TextEditor", "'gnome-text-editor %u'", 1000},
        {b_txt, "gnome-text-editor", "'gnome-text-editor %u'", 3000},
        {gone, "org.gnome.TextEditor", "'gnome-text-editor %u'", 4000},
        {c_png, "gimp", "'gimp-2.10 %U'", 5000},
        {a_txt, "gimp", "'gimp %U'", 6000},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(uses); i++) {
        char *uri = g_filename_to_uri(uses[i].path, nullptr, nullptr);
        GDateTime *when = g_date_time_new_from_unix_utc(uses[i].when);
        CHECK(g_bookmark_file_set_application_info(recent, uri, uses[i].app, uses[i].exec, 1,
                                                   when, nullptr));
        g_date_time_unref(when);
        g_free(uri);
    }
    char **got = xd_jumplist_recent(recent, "org.gnome.TextEditor.desktop",
                                    "/usr/bin/gnome-text-editor", 10);
    CHECK(g_strv_length(got) == 2);
    CHECK(got[0] != nullptr && g_str_has_suffix(got[0], "/b.txt"));
    CHECK(got[1] != nullptr && g_str_has_suffix(got[1], "/a.txt"));
    g_strfreev(got);
    got = xd_jumplist_recent(recent, "org.gnome.TextEditor.desktop", nullptr, 1);
    CHECK(g_strv_length(got) == 1 && g_str_has_suffix(got[0], "/a.txt")); /* by id alone */
    g_strfreev(got);
    got = xd_jumplist_recent(recent, "gimp.desktop", "gimp", 10);
    CHECK(g_strv_length(got) == 2 && g_str_has_suffix(got[0], "/a.txt"));
    g_strfreev(got);
    got = xd_jumplist_recent(recent, "firefox.desktop", "firefox", 10);
    CHECK(got[0] == nullptr);
    g_strfreev(got);
    got = xd_jumplist_recent(nullptr, "firefox.desktop", "firefox", 10);
    CHECK(got[0] == nullptr);
    g_strfreev(got);
    /* "Remove from this list": gone from the program's list, still in
     * GIMP's. */
    char *a_uri = g_filename_to_uri(a_txt, nullptr, nullptr);
    CHECK(xd_jumplist_forget(recent, a_uri, "org.gnome.TextEditor.desktop",
                             "/usr/bin/gnome-text-editor"));
    CHECK(!xd_jumplist_forget(recent, a_uri, "org.gnome.TextEditor.desktop",
                              "/usr/bin/gnome-text-editor"));
    got = xd_jumplist_recent(recent, "org.gnome.TextEditor.desktop",
                             "/usr/bin/gnome-text-editor", 10);
    CHECK(g_strv_length(got) == 1 && g_str_has_suffix(got[0], "/b.txt"));
    g_strfreev(got);
    got = xd_jumplist_recent(recent, "gimp.desktop", "gimp", 10);
    CHECK(g_strv_length(got) == 2 && g_str_has_suffix(got[0], "/a.txt"));
    g_strfreev(got);
    g_free(a_uri);
    g_bookmark_file_free(recent);
    g_remove(a_txt);
    g_remove(b_txt);
    g_remove(c_png);
    g_rmdir(dir);
    g_free(a_txt);
    g_free(b_txt);
    g_free(c_png);
    g_free(gone);
    g_free(dir);

    /* Dragging a pinned program along. */
    static char ids[4][2] = {"a", "b", "c", "d"};
    void *items[] = {ids[0], ids[1], ids[2], ids[3]};
    CHECK(xd_jumplist_move(items, 4, 3, 1));
    CHECK(items[0] == ids[0] && items[1] == ids[3] && items[2] == ids[1] && items[3] == ids[2]);
    CHECK(xd_jumplist_move(items, 4, 0, 3));
    CHECK(items[0] == ids[3] && items[1] == ids[1] && items[2] == ids[2] && items[3] == ids[0]);
    CHECK(!xd_jumplist_move(items, 4, 1, 1) && !xd_jumplist_move(items, 4, 4, 0));

    /* Files pinned to a jump list: in the order pinned, kept in the key
     * file across a save and load, and left out of Recent. */
    GKeyFile *kf = g_key_file_new();
    char **none = xd_jumplist_pinned(kf, "gedit.desktop");
    CHECK(none != nullptr && none[0] == nullptr);
    g_strfreev(none);
    CHECK(xd_jumplist_set_pinned(kf, "gedit.desktop", "file:///a.txt", true));
    CHECK(xd_jumplist_set_pinned(kf, "gedit.desktop", "file:///b;c.txt", true));
    CHECK(!xd_jumplist_set_pinned(kf, "gedit.desktop", "file:///a.txt", true));
    char *data = g_key_file_to_data(kf, nullptr, nullptr);
    GKeyFile *again = g_key_file_new();
    CHECK(g_key_file_load_from_data(again, data, (gsize)-1, G_KEY_FILE_NONE, nullptr));
    char **pinned = xd_jumplist_pinned(again, "gedit.desktop");
    CHECK(g_strv_length(pinned) == 2 && strcmp(pinned[0], "file:///a.txt") == 0 &&
          strcmp(pinned[1], "file:///b;c.txt") == 0);
    char **recent_uris = g_strsplit("file:///b;c.txt|file:///d.txt|file:///a.txt", "|", -1);
    xd_jumplist_drop_pinned(recent_uris, pinned);
    CHECK(g_strv_length(recent_uris) == 1 && strcmp(recent_uris[0], "file:///d.txt") == 0);
    g_strfreev(recent_uris);
    g_strfreev(pinned);
    g_key_file_free(again);
    g_free(data);
    /* Dragged within Pinned: b;c above a, and back. */
    CHECK(xd_jumplist_move_pinned(kf, "gedit.desktop", "file:///b;c.txt", "file:///a.txt"));
    pinned = xd_jumplist_pinned(kf, "gedit.desktop");
    CHECK(g_strv_length(pinned) == 2 && strcmp(pinned[0], "file:///b;c.txt") == 0);
    g_strfreev(pinned);
    CHECK(!xd_jumplist_move_pinned(kf, "gedit.desktop", "file:///a.txt", "file:///a.txt"));
    CHECK(!xd_jumplist_move_pinned(kf, "gedit.desktop", "file:///x.txt", "file:///a.txt"));
    CHECK(xd_jumplist_move_pinned(kf, "gedit.desktop", "file:///b;c.txt", "file:///a.txt"));
    CHECK(xd_jumplist_set_pinned(kf, "gedit.desktop", "file:///a.txt", false));
    CHECK(xd_jumplist_set_pinned(kf, "gedit.desktop", "file:///b;c.txt", false));
    CHECK(!g_key_file_has_group(kf, "gedit.desktop"));
    CHECK(!xd_jumplist_set_pinned(kf, "gedit.desktop", "file:///a.txt", false));
    g_key_file_free(kf);

    /* "Combine when taskbar is full": the least recently used program with
     * more than one window first, then the next. */
    size_t wins[] = {3, 1, 2, 4};
    uint64_t used[] = {50, 5, 20, 90};
    bool combined[] = {false, false, false, false};
    CHECK(xd_jumplist_combine_next(wins, used, combined, 4) == 2);
    combined[2] = true;
    CHECK(xd_jumplist_combine_next(wins, used, combined, 4) == 0);
    combined[0] = true;
    CHECK(xd_jumplist_combine_next(wins, used, combined, 4) == 3);
    combined[3] = true;
    CHECK(xd_jumplist_combine_next(wins, used, combined, 4) == SIZE_MAX);

    /* A media player is the program whose desktop file or app id it names. */
    CHECK(xd_jumplist_player_of("vlc", "org.mpris.MediaPlayer2.vlc", "vlc.desktop", "vlc"));
    CHECK(xd_jumplist_player_of("org.gnome.Totem", nullptr, "org.gnome.Totem.desktop", nullptr));
    CHECK(xd_jumplist_player_of("mpv", nullptr, "/home/u/Desktop/mpv.desktop", nullptr));
    CHECK(xd_jumplist_player_of("", "org.mpris.MediaPlayer2.vlc.instance42", nullptr, "vlc"));
    CHECK(!xd_jumplist_player_of("vlc", "org.mpris.MediaPlayer2.vlc", "mpv.desktop", "mpv"));
    CHECK(!xd_jumplist_player_of("vl", nullptr, "vlc.desktop", nullptr));
    CHECK(!xd_jumplist_player_of(nullptr, "org.example.Other", "vlc.desktop", "vlc"));
    CHECK(!xd_jumplist_player_of(nullptr, nullptr, "vlc.desktop", "vlc"));
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

/* xd-search: part of a name or a wildcard pattern, ignoring case; text
 * in a file's bytes. */
/* Task Manager: Networking's adapters from a sysfs-like tree, and Peak
 * Mem Usage's VmHWM. */
static void put_attr(const char *root, const char *rel, const char *text) {
    char *path = g_build_filename(root, rel, nullptr);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    CHECK(g_file_set_contents(path, text, -1, nullptr));
    g_free(dir);
    g_free(path);
}

static void test_taskmgr_netstat(void) {
    char *root = g_dir_make_tmp("xd-net-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    /* lo: skipped. eth0: up at 1 Gb/s. wlan0: up, no speed. eth1: cable
     * out. br0: disabled. vpn0: up, link unknown (tun). */
    put_attr(root, "lo/type", "772\n");
    put_attr(root, "lo/operstate", "unknown\n");
    put_attr(root, "lo/flags", "0x9\n");
    put_attr(root, "eth0/type", "1\n");
    put_attr(root, "eth0/operstate", "up\n");
    put_attr(root, "eth0/flags", "0x1003\n");
    put_attr(root, "eth0/speed", "1000\n");
    put_attr(root, "eth0/statistics/rx_bytes", "1000000\n");
    put_attr(root, "eth0/statistics/tx_bytes", "250000\n");
    put_attr(root, "wlan0/type", "1\n");
    put_attr(root, "wlan0/operstate", "up\n");
    put_attr(root, "wlan0/flags", "0x1003\n");
    put_attr(root, "wlan0/speed", "-1\n");
    put_attr(root, "eth1/type", "1\n");
    put_attr(root, "eth1/operstate", "down\n");
    put_attr(root, "eth1/flags", "0x1003\n");
    put_attr(root, "br0/type", "1\n");
    put_attr(root, "br0/operstate", "down\n");
    put_attr(root, "br0/flags", "0x1002\n");
    put_attr(root, "vpn0/type", "65534\n");
    put_attr(root, "vpn0/operstate", "unknown\n");
    put_attr(root, "vpn0/flags", "0x10d1\n");
    GArray *ifs = xd_netifs_read(root);
    CHECK(ifs->len == 5);
    if (ifs->len == 5) {
        const xd_netif *n = (const xd_netif *)(const void *)ifs->data;
        CHECK(strcmp(n[0].name, "br0") == 0 && n[0].state == XD_NETIF_NON_OPERATIONAL);
        CHECK(strcmp(n[1].name, "eth0") == 0 && n[1].state == XD_NETIF_OPERATIONAL &&
              n[1].speed_mbps == 1000 && n[1].rx_bytes == 1000000 && n[1].tx_bytes == 250000);
        CHECK(strcmp(n[2].name, "eth1") == 0 && n[2].state == XD_NETIF_DISCONNECTED &&
              n[2].speed_mbps == 0);
        CHECK(strcmp(n[3].name, "vpn0") == 0 && n[3].state == XD_NETIF_OPERATIONAL);
        CHECK(strcmp(n[4].name, "wlan0") == 0 && n[4].speed_mbps == 0);
    }
    xd_netifs_free(ifs);
    rm_tree(root);
    g_free(root);
    ifs = xd_netifs_read("/nonexistent");
    CHECK(ifs->len == 0);
    xd_netifs_free(ifs);

    CHECK(xd_netif_state_from("dormant", TRUE) == XD_NETIF_CONNECTING);
    CHECK(xd_netif_state_from("up", FALSE) == XD_NETIF_NON_OPERATIONAL);
    CHECK(strcmp(xd_netif_state_text(XD_NETIF_OPERATIONAL), "Operational") == 0);
    CHECK(strcmp(xd_netif_state_text(XD_NETIF_NON_OPERATIONAL), "Non Operational") == 0);

    static const struct {
        guint64 mbps;
        const char *text;
    } speeds[] = {{0, ""}, {10, "10 Mbps"}, {100, "100 Mbps"}, {1000, "1 Gbps"},
                  {2500, "2.5 Gbps"}, {10000, "10 Gbps"}};
    for (size_t i = 0; i < G_N_ELEMENTS(speeds); i++) {
        char *t = xd_netif_speed_text(speeds[i].mbps);
        CHECK(strcmp(t, speeds[i].text) == 0);
        g_free(t);
    }
    /* 12.5 MB in 1 s on 100 Mb/s is all of it; 125 kB is 1 %. */
    CHECK(xd_netif_utilization(12500000, 1.0, 100) == 1.0);
    double one_pct = xd_netif_utilization(125000, 1.0, 100);
    CHECK(one_pct > 0.0099999 && one_pct < 0.0100001);
    CHECK(xd_netif_utilization(1000, 1.0, 0) == 0 && xd_netif_utilization(1000, 0, 100) == 0);
    CHECK(xd_netif_utilization(G_MAXUINT64 / 16, 1.0, 10) == 1.0);
    static const struct {
        double share;
        const char *text;
    } utils[] = {{0, "0 %"}, {0.00001, "0 %"}, {0.0012, "0.12 %"}, {0.015, "1.5 %"},
                 {0.37, "37 %"}, {1.0, "100 %"}};
    for (size_t i = 0; i < G_N_ELEMENTS(utils); i++) {
        char *t = xd_netif_utilization_text(utils[i].share);
        CHECK(strcmp(t, utils[i].text) == 0);
        g_free(t);
    }

    CHECK(xd_status_peak_kb("Name:\tbash\nVmPeak:\t  20000 kB\nVmHWM:\t    5120 kB\n"
                            "VmRSS:\t    4096 kB\n") == 5120);
    CHECK(xd_status_peak_kb("Name:\tkthreadd\nThreads:\t1\n") == 0);
    CHECK(xd_status_peak_kb("") == 0);
    /* Our own: at least what we use now. */
    CHECK(xd_proc_peak_kb((int)getpid()) > 0);
    CHECK(xd_proc_peak_kb(-1) == 0);
}

static void test_search_match(void) {
    xd_search_name n;
    xd_search_name_init(&n, "  ");
    CHECK(xd_search_name_matches(&n, "anything"));
    xd_search_name_clear(&n);
    xd_search_name_init(&n, "Report");
    CHECK(xd_search_name_matches(&n, "Annual report 2026.odt"));
    CHECK(xd_search_name_matches(&n, "REPORT"));
    CHECK(!xd_search_name_matches(&n, "Repo.txt"));
    xd_search_name_clear(&n);
    xd_search_name_init(&n, "*.TXT");
    CHECK(xd_search_name_matches(&n, "notes.txt"));
    CHECK(!xd_search_name_matches(&n, "notes.txt.bak"));
    xd_search_name_clear(&n);
    xd_search_name_init(&n, "l?tter*");
    CHECK(xd_search_name_matches(&n, "Letter.txt"));
    CHECK(!xd_search_name_matches(&n, "a Letter.txt"));
    xd_search_name_clear(&n);
    xd_search_name_init(&n, "\u00FCn\u00EF");
    CHECK(xd_search_name_matches(&n, "\u00DCN\u00CFCODE"));
    xd_search_name_clear(&n);
    CHECK(n.part == nullptr && n.glob == nullptr);

    static const char data[] = "Dear Sir,\nShopping\0list";
    CHECK(xd_search_text_in(data, sizeof(data) - 1, "dear sir"));
    CHECK(xd_search_text_in(data, sizeof(data) - 1, "LIST"));
    CHECK(xd_search_text_in(data, sizeof(data) - 1, ""));
    CHECK(!xd_search_text_in(data, sizeof(data) - 1, "shopping list"));
    CHECK(!xd_search_text_in("ab", 2, "abc"));
}

/* The freedesktop thumbnail cache: MD5-of-URI names, the sizes' folders,
 * the PNG's Thumb:: metadata, and failure markers. */
static void test_thumbnail_cache(void) {
    /* The spec's own example. */
    char *p = xd_thumb_path("/c", "file:///home/jens/photos/me.png", XD_THUMB_NORMAL);
    CHECK(strcmp(p, "/c/normal/c6ee772d9e49320e97ec29a7eb5b1697.png") == 0);
    g_free(p);
    p = xd_thumb_path("/c", "file:///home/jens/photos/me.png", XD_THUMB_XXLARGE);
    CHECK(strcmp(p, "/c/xx-large/c6ee772d9e49320e97ec29a7eb5b1697.png") == 0);
    g_free(p);
    p = xd_thumb_fail_path("/c", "file:///home/jens/photos/me.png");
    CHECK(strcmp(p, "/c/fail/xd-thumbnailer-" XD_VERSION "/c6ee772d9e49320e97ec29a7eb5b1697.png") ==
          0);
    g_free(p);
    CHECK(xd_thumb_size_for(48) == XD_THUMB_NORMAL && xd_thumb_size_for(128) == XD_THUMB_NORMAL);
    CHECK(xd_thumb_size_for(129) == XD_THUMB_LARGE && xd_thumb_size_for(248) == XD_THUMB_LARGE);
    CHECK(xd_thumb_size_for(600) == XD_THUMB_XXLARGE && xd_thumb_size_for(5000) == XD_THUMB_XXLARGE);
    XdThumbSize size = XD_THUMB_NORMAL;
    CHECK(xd_thumb_size_parse("x-large", &size) && size == XD_THUMB_XLARGE);
    CHECK(!xd_thumb_size_parse("huge", &size) && size == XD_THUMB_XLARGE);
    CHECK(xd_thumb_size_px(XD_THUMB_LARGE) == 256 && strcmp(xd_thumb_size_name(XD_THUMB_LARGE), "large") == 0);
    CHECK(xd_thumb_in_cache("/c/thumbnails", "/c/thumbnails/large/x.png"));
    CHECK(xd_thumb_in_cache("/c/thumbnails/", "/c/thumbnails/../thumbnails/normal/x.png"));
    CHECK(!xd_thumb_in_cache("/c/thumbnails", "/c/thumbnails-old/x.png"));
    CHECK(xd_thumb_pixbuf_reads("image/png") && xd_thumb_supported("image/png"));
    CHECK(!xd_thumb_supported("inode/directory"));

    char *root = g_dir_make_tmp("xd-thumbs-XXXXXX", nullptr);
    const char *uri = "file:///tmp/a%20picture.png";
    char *dest = xd_thumb_path(root, uri, XD_THUMB_LARGE);
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 40, 30);
    gdk_pixbuf_fill(pb, 0x336699ff);
    CHECK(xd_thumb_save(pb, dest, uri, 1700000000, 1234, "image/png", nullptr));
    g_object_unref(pb);
    GStatBuf st;
    CHECK(g_stat(dest, &st) == 0 && (st.st_mode & 0777) == 0600);
    char *dir = g_path_get_dirname(dest);
    CHECK(g_stat(dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    pb = gdk_pixbuf_new_from_file(dest, nullptr);
    CHECK(pb != nullptr);
    if (pb != nullptr) {
        CHECK(gdk_pixbuf_get_width(pb) == 40 && gdk_pixbuf_get_height(pb) == 30);
        CHECK(g_strcmp0(gdk_pixbuf_get_option(pb, "tEXt::Thumb::URI"), uri) == 0);
        CHECK(g_strcmp0(gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime"), "1700000000") == 0);
        CHECK(g_strcmp0(gdk_pixbuf_get_option(pb, "tEXt::Thumb::Size"), "1234") == 0);
        CHECK(g_strcmp0(gdk_pixbuf_get_option(pb, "tEXt::Thumb::Mimetype"), "image/png") == 0);
        g_object_unref(pb);
    }
    CHECK(xd_thumb_is_current(dest, uri, 1700000000));
    CHECK(!xd_thumb_is_current(dest, uri, 1700000001)); /* the file changed */
    CHECK(!xd_thumb_is_current(dest, "file:///tmp/other.png", 1700000000));

    CHECK(!xd_thumb_has_failed(root, uri, 1700000000));
    CHECK(xd_thumb_mark_failed(root, uri, 1700000000, nullptr));
    CHECK(xd_thumb_has_failed(root, uri, 1700000000));
    CHECK(!xd_thumb_has_failed(root, uri, 1700000005)); /* changed since: try again */
    char *fail = xd_thumb_fail_path(root, uri);
    CHECK(g_file_test(fail, G_FILE_TEST_IS_REGULAR));

    char *fail_dir = g_path_get_dirname(fail);
    char *fail_parent = g_path_get_dirname(fail_dir);
    CHECK(g_remove(fail) == 0 && g_rmdir(fail_dir) == 0 && g_rmdir(fail_parent) == 0);
    CHECK(g_remove(dest) == 0 && g_rmdir(dir) == 0 && g_rmdir(root) == 0);
    g_free(fail_parent);
    g_free(fail_dir);
    g_free(fail);
    g_free(dir);
    g_free(dest);
    g_free(root);
}

/* xd-files' Favorites: GTK's bookmarks file, other lines kept as they are. */
static void test_favorites(void) {
    char *dir = g_dir_make_tmp("xd-favorites-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "gtk-3.0", "bookmarks", nullptr);
    GPtrArray *none = xd_favorites_load(path);
    CHECK(none->len == 0);
    g_ptr_array_unref(none);
    /* Adding creates the folder and file. */
    CHECK(xd_favorites_add(path, "file:///home/me/Music", nullptr, nullptr));
    char *gtk_dir = g_path_get_dirname(path);
    CHECK(g_file_test(gtk_dir, G_FILE_TEST_IS_DIR));
    static const char start[] = "# made by hand\n"
                                "file:///home/me/My%20Work Work stuff\r\n"
                                "\n"
                                "not a bookmark line\n"
                                "sftp://example.org/srv/data\n"
                                "smb://server/\n";
    CHECK(g_file_set_contents(path, start, -1, nullptr));
    GPtrArray *f = xd_favorites_load(path);
    CHECK(f->len == 3);
    if (f->len == 3) {
        XdFavorite *a = g_ptr_array_index(f, 0), *b = g_ptr_array_index(f, 1),
                   *c = g_ptr_array_index(f, 2);
        CHECK(strcmp(a->uri, "file:///home/me/My%20Work") == 0);
        CHECK(g_strcmp0(a->label, "Work stuff") == 0);
        CHECK(b->label == nullptr);
        char *name = xd_favorite_name(b);
        CHECK(strcmp(name, "data") == 0);
        g_free(name);
        name = xd_favorite_name(c);
        CHECK(strcmp(name, "server") == 0);
        g_free(name);
        g_clear_pointer(&a->label, g_free);
        name = xd_favorite_name(a);
        CHECK(strcmp(name, "My Work") == 0);
        g_free(name);
    }
    g_ptr_array_unref(f);

    CHECK(xd_favorites_add(path, "file:///tmp", "Scratch", nullptr));
    CHECK(xd_favorites_add(path, "sftp://example.org/srv/data", "Data\nServer", nullptr));
    CHECK(xd_favorites_rename(path, "file:///home/me/My%20Work", "", nullptr));
    CHECK(xd_favorites_remove(path, "smb://server/", nullptr));
    const char *order[] = {"file:///tmp", "file:///home/me/My%20Work", nullptr};
    CHECK(xd_favorites_reorder(path, order, nullptr));
    char *text = nullptr;
    CHECK(g_file_get_contents(path, &text, nullptr, nullptr));
    CHECK(g_strcmp0(text, "# made by hand\n"
                          "file:///tmp Scratch\n"
                          "\n"
                          "not a bookmark line\n"
                          "file:///home/me/My%20Work\n"
                          "sftp://example.org/srv/data Data Server\n") == 0);
    g_free(text);
    CHECK(g_remove(path) == 0);
    CHECK(g_rmdir(gtk_dir) == 0);
    CHECK(g_rmdir(dir) == 0);
    g_free(gtk_dir);
    g_free(path);
    g_free(dir);
}

/* The Shortcut tab: Exec, Path, Comment and Icon; a system launcher is
 * changed as a user copy, everything else in it kept. */
static void test_launcher_fields(void) {
    char *dir = g_dir_make_tmp("xd-launcher-XXXXXX", nullptr);
    char *sys = g_build_filename(dir, "sys", nullptr);
    char *user = g_build_filename(dir, "user", "applications", nullptr);
    CHECK(g_mkdir(sys, 0755) == 0);
    char *path = g_build_filename(sys, "editor.desktop", nullptr);
    static const char entry[] = "# keep me\n[Desktop Entry]\nType=Application\nName=Editor\n"
                                "Name[fr]=Éditeur\nExec=editor %F\nComment=Edits text\n"
                                "Comment[xx_YY]=Other\nIcon=accessories-text-editor\n"
                                "Path=/srv\n\n[Desktop Action new]\nName=New\nExec=editor -n\n";
    CHECK(g_file_set_contents(path, entry, -1, nullptr));
    XdLauncher l;
    CHECK(xd_launcher_read(path, &l, nullptr));
    CHECK(g_strcmp0(l.target, "editor %F") == 0 && g_strcmp0(l.start_in, "/srv") == 0);
    CHECK(g_strcmp0(l.comment, "Edits text") == 0 && !l.is_link);
    CHECK(g_strcmp0(l.icon, "accessories-text-editor") == 0);
    g_free(l.target);
    l.target = g_strdup("editor --new-window %F");
    g_free(l.start_in);
    l.start_in = g_strdup("");
    g_free(l.comment);
    l.comment = g_strdup("Writes notes");
    /* Read-only (a system launcher): the change goes to a user copy. Not
     * as root (package builds in containers), which writes anyway. */
    CHECK(g_chmod(sys, 0555) == 0);
    bool read_only = access(sys, W_OK) != 0;
    if (!read_only)
        g_printerr("test_launcher_fields: running as root, skipped the read-only launcher\n");
    char *written = read_only ? xd_launcher_write(path, &l, user, nullptr) : nullptr;
    char *want = g_build_filename(user, "editor.desktop", nullptr);
    if (read_only) {
        CHECK(g_strcmp0(written, want) == 0);
        XdLauncher back;
        CHECK(written != nullptr && xd_launcher_read(written, &back, nullptr));
        CHECK(g_strcmp0(back.target, "editor --new-window %F") == 0 && back.start_in == nullptr);
        CHECK(g_strcmp0(back.comment, "Writes notes") == 0);
        xd_launcher_clear(&back);
        GKeyFile *kf = g_key_file_new();
        CHECK(written != nullptr &&
              g_key_file_load_from_file(kf, written, G_KEY_FILE_KEEP_TRANSLATIONS, nullptr));
        char *fr = g_key_file_get_string(kf, "Desktop Entry", "Name[fr]", nullptr);
        char *other = g_key_file_get_string(kf, "Desktop Entry", "Comment[xx_YY]", nullptr);
        char *action = g_key_file_get_string(kf, "Desktop Action new", "Exec", nullptr);
        CHECK(g_strcmp0(fr, "Éditeur") == 0 && g_strcmp0(other, "Other") == 0);
        CHECK(g_strcmp0(action, "editor -n") == 0);
        g_free(fr);
        g_free(other);
        g_free(action);
        g_key_file_unref(kf);
        /* The original is untouched. */
        char *orig = nullptr;
        CHECK(g_file_get_contents(path, &orig, nullptr, nullptr) && strcmp(orig, entry) == 0);
        g_free(orig);
    }
    /* Writable: in place. */
    CHECK(g_chmod(sys, 0755) == 0);
    char *again = xd_launcher_write(path, &l, user, nullptr);
    CHECK(g_strcmp0(again, path) == 0);
    g_free(again);
    xd_launcher_clear(&l);

    char *dir_entry = g_build_filename(sys, "folder.desktop", nullptr);
    CHECK(g_file_set_contents(dir_entry, "[Desktop Entry]\nType=Directory\nName=X\n", -1,
                              nullptr));
    CHECK(!xd_launcher_read(dir_entry, &l, nullptr));
    char *link = g_build_filename(sys, "site.desktop", nullptr);
    CHECK(g_file_set_contents(link, "[Desktop Entry]\nType=Link\nName=Site\nURL=https://a.b/\n",
                              -1, nullptr));
    CHECK(xd_launcher_read(link, &l, nullptr) && l.is_link &&
          g_strcmp0(l.target, "https://a.b/") == 0);
    xd_launcher_clear(&l);

    CHECK(g_remove(link) == 0 && g_remove(dir_entry) == 0 && g_remove(path) == 0);
    if (read_only) {
        char *user_parent = g_path_get_dirname(user);
        CHECK(g_remove(want) == 0 && g_rmdir(user) == 0 && g_rmdir(user_parent) == 0);
        g_free(user_parent);
    }
    CHECK(g_rmdir(sys) == 0 && g_rmdir(dir) == 0);
    g_free(link);
    g_free(dir_entry);
    g_free(written);
    g_free(want);
    g_free(path);
    g_free(user);
    g_free(sys);
    g_free(dir);
}

int main(void) {
    test_str();
    test_arena();
    test_scheme();
    test_shipped_schemes();
    test_luna_variants();
    test_px();
    test_render();
    test_hit();
    test_apps();
    test_menus();
    test_textedit();
    test_runcmd();
    test_settings();
    test_glass_color();
    test_saver_settings();
    test_savers();
    test_desktop_icon_settings();
    test_tray_rules();
    test_power_settings();
    test_battery_alarms();
    test_access_extra_settings();
    test_power_schemes();
    test_sound_scheme();
    test_gtk_css();
    test_passwd();
    test_intl();
    test_intl_custom();
    test_users();
    test_folder_options();
    test_input_settings();
    test_kbd_layouts();
    test_cursor_themes();
    test_file_types_edit();
    test_theme_settings();
    test_qt_style();
    test_qt_palette();
    test_fonts();
    test_bluetooth_info();
    test_joystick();
    test_bluetooth_settings();
    test_netconn();
    test_admin_services();
    test_admin_events();
    test_admin_disks();
    test_appwiz();
    test_superbar();
    test_printers();
    test_scanners();
    test_schedtasks();
    test_search_match();
    test_thumbnail_cache();
    test_favorites();
    test_launcher_fields();
    test_taskmgr_netstat();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
