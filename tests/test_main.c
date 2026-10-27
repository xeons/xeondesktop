/* SPDX-License-Identifier: MIT
 * Unit tests for the core library and theme engine.
 */
#include <glib/gstdio.h>
#include <linux/input-event-codes.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <unistd.h>

#include <crypt.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <math.h>
#include <monetary.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <utime.h>
#include <sys/wait.h>

#include <fontconfig/fontconfig.h>
#ifdef XD_HAVE_LCMS2
#include <lcms2.h>
#endif

#include "../src/admin/admin.h"
#include "../src/appwiz/programs.h"
#include "../src/flatpak/perms.h"
#include "../src/apps/runcmd.h"
#include "../src/power/schemes.h"
#include "../src/power/upsstate.h"
#include "../src/printers/cupsops.h"
#include "../src/privacy/permissions.h"
#include "../src/printers/printlib.h"
#include "../src/scanners/scanlib.h"
#include "../src/bluetooth/btinfo.h"
#include "../src/joy/joyinfo.h"
#include "../src/keys/gpgkeys.h"
#include "../src/keys/keyslib.h"
#include "../src/keys/sshkeys.h"
#include "../src/netconn/netconn.h"
#include "../src/intl/intl.h"
#include "../src/keyboard/layouts.h"
#include "../src/mouse/cursors.h"
#include "../src/files/extract.h"
#if XD_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif
#include "../src/files/cleanup.h"
#include "../src/files/syscleanup.h"
#include "../src/files/columns.h"
#include "../src/files/favorites.h"
#include "../src/files/launcher.h"
#include "../src/files/luks.h"
#include "../src/files/names.h"
#include "../src/files/netdrives.h"
#include "../src/files/usershare.h"
#include "../src/files/versions.h"
#include "../src/portal/chooserlib.h"
#include "../src/folders/filetypes.h"
#include "../src/fonts/fontlist.h"
#include "../src/schedtasks/task.h"
#include "../src/security/passwd.h"
#include "../src/saver/savers.h"
#include "../src/color/colorlib.h"
#include "../src/compositor/vcgt.h"
#include "../src/settings/gtktheme.h"
#include "../src/settings/inputmethod.h"
#include "../src/compositor/grabkeys.h"
#include "../src/compositor/imeplace.h"
#include "../src/compositor/present.h"
#include "../src/settings/nightlight.h"
#include "../src/settings/qtpalette.h"
#include "../src/settings/settings.h"
#include "../src/settings/sounds.h"
#include "../src/settings/themepkg.h"
#include "../src/settings/themeset.h"
#include "../src/settings/themes.h"
/* For GSettings in memory, never the user's dconf. */
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <json-glib/json-glib.h>
#include "../src/shell/apps.h"
#include "../src/shell/gpu.h"
#include "../src/startup/startup.h"
#include "../src/startmenu/searchlib.h"
#include "../src/taskmgr/netstat.h"
#include "../src/timedate/ntp.h"
#include "../src/timedate/zone.h"
#include "../src/shell/calendar.h"
#include "../src/shell/notices.h"
#include <wayland-client-protocol.h>

#include "../src/mines/minefield.h"
#include "../src/vboxdnd/dnd.h"
#include "../src/vboxdnd/fakehost.h"
#include "../src/snip/recording.h"
#include "../src/snip/snipping.h"
#include "../src/shell/jumplist.h"
#include "../src/shell/launcher.h"
#include "../src/shell/recent.h"
#include "../src/ui/textedit.h"
#include "../src/users/accounts.h"
#include "../src/users/users.h"
#include "../src/theme/pkg.h"
#include "../src/theme/theme.h"
#include "../src/theme/gtkpkg.h"
#include "../src/theme/pkg.h"
#include "../src/theme/scale.h"
#include "../src/theme/panes.h"
#include "../src/theme/pkg.h"
#include "../src/thumbnailer/thumbcache.h"
#include "../src/wuaucpl/state.h"
#include "../src/wuaucpl/updlib.h"

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

    int64_t value = 0;
    CHECK(xd_str_parse_i64(XD_STR(" 255 "), 0, 255, &value) && value == 255);
    CHECK(!xd_str_parse_i64(XD_STR("256"), 0, 255, &value));
    CHECK(!xd_str_parse_i64(XD_STR("12x"), 0, 255, &value));
    CHECK(!xd_str_parse_i64(XD_STR("-"), -10, 10, &value));
    CHECK(xd_str_parse_i64(XD_STR("-9223372036854775808"), INT64_MIN, INT64_MAX, &value) &&
          value == INT64_MIN);
    CHECK(!xd_str_parse_i64(XD_STR("9223372036854775808"), INT64_MIN, INT64_MAX, &value));

    double number = 0;
    CHECK(xd_str_parse_double(XD_STR("1.25"), &number) && number > 1.249 && number < 1.251);
    CHECK(!xd_str_parse_double(XD_STR("1.25x"), &number));

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
    xd_arena arena;
    xd_arena_init(&arena, 256);
    int *ints = XD_ARENA_ARRAY(&arena, int, 10);
    for (int i = 0; i < 10; i++)
        CHECK(ints[i] == 0);
    double *dbl = XD_ARENA_NEW(&arena, double);
    CHECK(((uintptr_t)dbl % alignof(double)) == 0);
    /* Larger than a block: must get its own block. */
    char *big = xd_arena_alloc(&arena, 10000, 1);
    big[9999] = 'x';
    char *text = xd_str_arena_cstr(&arena, XD_STR("hello"));
    CHECK(strcmp(text, "hello") == 0);
    xd_arena_destroy(&arena);
    CHECK(arena.head == nullptr);
}

static void test_scheme(void) {
    xd_scheme scheme;
    xd_strbuf err = {0};

    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nName=Test\n[Colors]\nActiveTitle=1 2 3\n"
                                 "Background = 4,5,6\n[Metrics]\nCaptionHeight=25\n"
                                 "[Fonts]\nCaptionFont=Arial, 10, bold, italic\n"),
                          &scheme, &err));
    CHECK(strcmp(scheme.name, "Test") == 0);
    CHECK(scheme.colors[XD_COLOR_ACTIVE_TITLE].r == 1 &&
          scheme.colors[XD_COLOR_ACTIVE_TITLE].b == 3);
    CHECK(scheme.colors[XD_COLOR_BACKGROUND].g == 5);
    CHECK(scheme.metrics[XD_METRIC_CAPTION_HEIGHT] == 25);
    CHECK(strcmp(scheme.fonts[XD_FONT_CAPTION].family, "Arial") == 0);
    CHECK(scheme.fonts[XD_FONT_CAPTION].bold && scheme.fonts[XD_FONT_CAPTION].italic);
    /* Unlisted colours keep the Windows Standard defaults. */
    CHECK(scheme.colors[XD_COLOR_BUTTON_FACE].r == 212);

    static const char *const bad[] = {
        "[Colors]\nActiveTitle=1 2\n",       "[Colors]\nActiveTitle=1 2 3 4\n",
        "[Colors]\nActiveTitle=256 0 0\n",   "[Colors]\nNoSuchColor=1 2 3\n",
        "[Metrics]\nCaptionHeight=-1\n",     "[Fonts]\nCaptionFont=Arial\n",
        "[Fonts]\nCaptionFont=Arial, 10, x\n", "[Bogus]\nk=v\n",
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(bad); i++) {
        xd_strbuf_clear(&err);
        CHECK(!xd_scheme_parse(xd_str_from_cstr(bad[i]), &scheme, &err));
        CHECK(err.len > 0);
        /* A failed parse leaves the defaults in place. */
        CHECK(strcmp(scheme.name, "Windows Standard") == 0);
    }
    xd_strbuf_free(&err);
}

static bool load_shipped_scheme(const char *stem, xd_scheme *scheme) {
    xd_strbuf path = {0};
    xd_strbuf_appendf(&path, "%s/data/schemes/%s.ini", XD_SOURCE_ROOT, stem);
    xd_strbuf err = {0};
    bool ok = xd_scheme_load(xd_strbuf_cstr(&path), scheme, &err);
    if (!ok)
        fprintf(stderr, "%s: %s\n", stem, xd_strbuf_cstr(&err));
    xd_strbuf_free(&err);
    xd_strbuf_free(&path);
    return ok;
}

static bool rgb_is(xd_rgb color, uint8_t red, uint8_t green, uint8_t blue) {
    return color.r == red && color.g == green && color.b == blue;
}

static void collect_custom(void *user, const char *key, const char *value) {
    xd_strbuf_appendf(user, "%s=%s\n", key, value);
}

/* Advanced Appearance's changes: set by scheme-file key, written back the
 * same way, laid over a scheme, and carried in desktop.ini's
 * [CustomScheme] through a dialog's merge. */
static void test_scheme_custom(void) {
    xd_scheme_custom custom = {0};
    CHECK(xd_scheme_custom_set(&custom, XD_STR("ActiveTitle"), XD_STR("128 0 0")));
    CHECK(xd_scheme_custom_set(&custom, XD_STR("captionheight"), XD_STR("25")));
    CHECK(xd_scheme_custom_set(&custom, XD_STR("MenuFont"),
                               XD_STR("DejaVu Serif, Bitstream, 12.5, italic")));
    CHECK(xd_scheme_custom_set(&custom, XD_STR("SmCaptionFont"), XD_STR("Arial, 7, bold")));
    CHECK(xd_scheme_custom_set(&custom, XD_STR("IconVerticalSpacing"), XD_STR("90")));
    /* Bad keys and values change nothing. */
    xd_scheme_custom before = custom;
    CHECK(!xd_scheme_custom_set(&custom, XD_STR("Bogus"), XD_STR("1")));
    CHECK(!xd_scheme_custom_set(&custom, XD_STR("Window"), XD_STR("1 2")));
    CHECK(!xd_scheme_custom_set(&custom, XD_STR("BorderWidth"), XD_STR("-3")));
    CHECK(!xd_scheme_custom_set(&custom, XD_STR("CaptionFont"), XD_STR("Arial")));
    CHECK(memcmp(&before, &custom, sizeof(custom)) == 0);
    CHECK(custom.color_mask == 1u << XD_COLOR_ACTIVE_TITLE);

    xd_strbuf out = {0};
    xd_scheme_custom_foreach(&custom, collect_custom, &out);
    CHECK(strcmp(xd_strbuf_cstr(&out),
                 "ActiveTitle=128 0 0\nCaptionHeight=25\nIconVerticalSpacing=90\n"
                 "MenuFont=DejaVu Serif, Bitstream, 12.5, italic\n"
                 "SmCaptionFont=Arial, 7, bold\n") == 0);
    xd_strbuf_free(&out);

    /* Laid over Windows Standard: only what was set changes. */
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    xd_scheme_apply_custom(&scheme, &custom);
    CHECK(rgb_is(scheme.colors[XD_COLOR_ACTIVE_TITLE], 128, 0, 0));
    CHECK(rgb_is(scheme.colors[XD_COLOR_BUTTON_FACE], 212, 208, 200));
    CHECK(scheme.metrics[XD_METRIC_CAPTION_HEIGHT] == 25 &&
          scheme.metrics[XD_METRIC_BORDER_WIDTH] == 1);
    CHECK(strcmp(scheme.fonts[XD_FONT_MENU].family, "DejaVu Serif, Bitstream") == 0);
    CHECK(scheme.fonts[XD_FONT_MENU].size_pt == 12.5 && scheme.fonts[XD_FONT_MENU].italic &&
          !scheme.fonts[XD_FONT_MENU].bold);
    CHECK(scheme.fonts[XD_FONT_SMCAPTION].bold && scheme.fonts[XD_FONT_SMCAPTION].size_pt == 7);

    /* Trimming drops what the scheme has anyway. */
    xd_scheme_custom changed = custom;
    CHECK(xd_scheme_custom_set(&changed, XD_STR("Background"), XD_STR("58 110 165")));
    CHECK(
        xd_scheme_custom_set(&changed, XD_STR("MessageFont"), XD_STR(XD_FONT_FAMILY_TAHOMA ", 8")));
    xd_scheme std;
    xd_scheme_init_default(&std);
    xd_scheme_custom_trim(&changed, &std);
    CHECK(changed.color_mask == custom.color_mask && changed.metric_mask == custom.metric_mask &&
          changed.font_mask == custom.font_mask);

    /* In desktop.ini, and back. */
    xd_settings original, parsed;
    xd_settings_defaults(&original);
    original.custom_scheme = custom;
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "[CustomScheme]\nActiveTitle=128 0 0\n") != nullptr);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed));
    CHECK(parsed.custom_scheme.font_mask == custom.font_mask &&
          parsed.custom_scheme.fonts[XD_FONT_MENU].size_pt == 12.5);
    xd_strbuf_free(&text);
    /* A bad value is left out, the rest kept. */
    xd_settings_parse(XD_STR("[CustomScheme]\nWindow=1 2 3\nMenu=oops\nCaptionHeight=x\n"),
                      &parsed);
    CHECK(parsed.custom_scheme.color_mask == 1u << XD_COLOR_WINDOW &&
          parsed.custom_scheme.metric_mask == 0);

    /* The scheme the settings select: the changes go over the font size
     * (as they were made, at that size), not under High Contrast. */
    original.font_size = XD_FONT_LARGE;
    xd_settings_scheme(&original, &scheme);
    CHECK(scheme.metrics[XD_METRIC_CAPTION_HEIGHT] == 25);
    CHECK(scheme.metrics[XD_METRIC_MENU_HEIGHT] == 23); /* 18 x 1.25, not changed */
    CHECK(rgb_is(scheme.colors[XD_COLOR_ACTIVE_TITLE], 128, 0, 0));
    original.access.high_contrast = true;
    xd_settings_scheme(&original, &scheme);
    CHECK(rgb_is(scheme.colors[XD_COLOR_ACTIVE_TITLE], 10, 36, 106));

    /* Apply carries them over another program's save, key by key; one
     * dropped since is removed. */
    xd_settings base, pending, now;
    xd_settings_defaults(&base);
    base.custom_scheme = custom;
    pending = base;
    pending.custom_scheme.color_mask = 0; /* ActiveTitle back to the scheme's */
    CHECK(xd_scheme_custom_set(&pending.custom_scheme, XD_STR("Window"), XD_STR("1 2 3")));
    now = base;
    now.style = XD_STYLE_WHISTLER;
    xd_settings_merge(&base, &pending, &now);
    CHECK(now.style == XD_STYLE_WHISTLER);
    CHECK(now.custom_scheme.color_mask == 1u << XD_COLOR_WINDOW);
    CHECK(now.custom_scheme.metric_mask == custom.metric_mask);

    /* A font's text reads back as itself, its size whatever the locale. */
    xd_font_spec font = {.family = "A, B", .size_pt = 9.5, .bold = true, .italic = true};
    char buf[256];
    xd_font_spec_format(&font, buf, sizeof(buf));
    CHECK(strcmp(buf, "A, B, 9.5, bold, italic") == 0);
}

static void test_shipped_schemes(void) {
    /* Every shipped scheme parses and has its own name. */
    xd_strbuf dir = {0};
    xd_strbuf_appendf(&dir, "%s/data/schemes", XD_SOURCE_ROOT);
    GDir *folder_handle = g_dir_open(xd_strbuf_cstr(&dir), 0, nullptr);
    CHECK(folder_handle != nullptr);
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    size_t count = 0;
    const char *file;
    while (folder_handle != nullptr && (file = g_dir_read_name(folder_handle)) != nullptr) {
        if (!g_str_has_suffix(file, ".ini"))
            continue;
        char *stem = g_strndup(file, strlen(file) - 4);
        xd_scheme scheme;
        CHECK(load_shipped_scheme(stem, &scheme));
        CHECK(g_hash_table_add(names, g_strdup(scheme.name)));
        g_free(stem);
        count++;
    }
    if (folder_handle != nullptr)
        g_dir_close(folder_handle);
    g_hash_table_destroy(names);
    xd_strbuf_free(&dir);
    /* XP's 22 classic schemes, Windows XP style's three colours,
     * Royale's Energy Blue and our Dark. */
    CHECK(count == 27);

    /* Programs are asked for their dark look with dark windows only. */
    static const struct {
        const char *stem;
        bool dark;
    } darkness[] = {
        {"luna-dark", true}, {"high-contrast-black", true}, {"luna-blue", false},
        {"windows-standard", false}, {"high-contrast-white", false},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(darkness); i++) {
        xd_scheme scheme;
        CHECK(load_shipped_scheme(darkness[i].stem, &scheme));
        CHECK(xd_scheme_is_dark(&scheme) == darkness[i].dark);
    }

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
        {.stem = "luna-dark", .name = "Dark",
         .color = XD_COLOR_WINDOW, .r = 30, .g = 30, .b = 32, .luna = XD_LUNA_DARK},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(want); i++) {
        xd_scheme scheme;
        CHECK(load_shipped_scheme(want[i].stem, &scheme));
        CHECK(strcmp(scheme.name, want[i].name) == 0);
        CHECK(rgb_is(scheme.colors[want[i].color], want[i].r, want[i].g, want[i].b));
        CHECK(scheme.luna_color == want[i].luna);
        CHECK(scheme.high_contrast == want[i].high_contrast);
    }
}

static uint32_t pixel(cairo_surface_t *surface, int x, int y) {
    cairo_surface_flush(surface);
    int stride = cairo_image_surface_get_stride(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    uint32_t px;
    memcpy(&px, data + (size_t)y * (size_t)stride + (size_t)x * 4, sizeof(px));
    return px & 0xFFFFFF;
}

static bool near(uint32_t got, uint32_t want, int tol) {
    for (int shift = 0; shift <= 16; shift += 8) {
        int got_channel = (int)((got >> shift) & 0xFF);
        int want_channel = (int)((want >> shift) & 0xFF);
        if (got_channel - want_channel > tol || want_channel - got_channel > tol)
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
        xd_theme theme;
        xd_theme_init(&theme, XD_STYLE_LUNA, &scheme, 1.0);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 200);
        cairo_t *cairo = cairo_create(surf);
        xd_frame frame = {
            .title = "", .active = true, .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MINIMIZE};
        xd_rect outer = {0, 0, 300, 200};
        xd_theme_draw_frame(&theme, cairo, outer, &frame);
        /* Row 8 of the measured caption is ActiveTitle. */
        CHECK(near(pixel(surf, 200, 8), variants[i].caption, 0));
        xd_frame_layout frame_layout;
        xd_theme_frame_layout(&theme, outer, &frame, &frame_layout);
        for (size_t button = 0; button < frame_layout.button_count; button++) {
            uint32_t px = pixel(surf, frame_layout.button_rects[button].x + 3,
                                frame_layout.button_rects[button].y + 10);
            if (frame_layout.button_parts[button] == XD_PART_CLOSE)
                close[i] = px;
            else
                minimize[i] = px;
        }
        cairo_destroy(cairo);
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

    /* Dark, ours: Blue's caption in charcoal with a little blue left,
     * XP's red Close kept and its green Start electric blue, a dark push
     * button with a dim grey outline (a focused one's ring blue), dark
     * Start menu lists without the orange line. */
    xd_scheme dk;
    CHECK(load_shipped_scheme("luna-dark", &dk));
    xd_theme_init(&st, XD_STYLE_LUNA, &dk, 1.0);
    sf.active = true;
    xd_theme_draw_frame(&st, scr, (xd_rect){0, 0, 300, 60}, &sf);
    uint32_t caption = pixel(ss, 150, 8);
    CHECK(near(caption, 0x1E2D47, 2));
    xd_frame_layout dark_layout;
    xd_theme_frame_layout(&st, (xd_rect){0, 0, 300, 60}, &sf, &dark_layout);
    uint32_t close_face =
        pixel(ss, dark_layout.button_rects[0].x + 3, dark_layout.button_rects[0].y + 10);
    CHECK(((close_face >> 16) & 0xFF) > ((close_face >> 8) & 0xFF) + 64);
    xd_theme_draw_start_button(&st, scr, (xd_rect){0, 60, 99, 30}, 0);
    uint32_t electric = pixel(ss, 4, 75);
    CHECK((electric & 0xFF) > ((electric >> 16) & 0xFF) + 80 &&
          (electric & 0xFF) > ((electric >> 8) & 0xFF) + 30);
    xd_theme_draw_button(&st, scr, (xd_rect){150, 60, 75, 23}, 0, "");
    /* The outline's first column is partly see-through: the second. */
    uint32_t button_face = pixel(ss, 187, 71), button_edge = pixel(ss, 151, 71);
    CHECK((button_face & 0xFF) < 80);
    CHECK(((button_edge >> 8) & 0xFF) > 80 && ((button_edge >> 8) & 0xFF) < 140 &&
          (button_edge & 0xFF) - ((button_edge >> 16) & 0xFF) < 30);
    xd_theme_draw_button(&st, scr, (xd_rect){150, 60, 75, 23}, XD_STATE_DEFAULT, "");
    uint32_t ring = pixel(ss, 152, 71); /* inside the outline */
    CHECK((ring & 0xFF) > ((ring >> 16) & 0xFF) + 40);
    xd_theme_luna_start_colors(&st, &sc);
    CHECK(sc.left_bg == 0x26272B && sc.left_text == 0xF0F0F0 && !sc.accent);
    xd_strbuf dark_css = {0};
    xd_theme_gtk_css(&st, &dark_css);
    CHECK(strstr(xd_strbuf_cstr(&dark_css), "@define-color theme_base_color #1e1e20;") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&dark_css), "@define-color theme_text_color #e6e6e6;") != nullptr);
    xd_strbuf_free(&dark_css);
    cairo_destroy(scr);
    cairo_surface_destroy(ss);

    /* The GTK theme follows the variant's selection colour. */
    xd_scheme olive;
    CHECK(load_shipped_scheme("luna-olive", &olive));
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_LUNA, &olive, 1.0);
    xd_strbuf css = {0};
    xd_theme_gtk_css(&theme, &css);
    CHECK(strstr(xd_strbuf_cstr(&css), "@define-color theme_selected_bg_color #93a070;") !=
          nullptr);
    xd_strbuf_free(&css);

    /* A bad LunaColor is refused. */
    xd_scheme parsed;
    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=homestead\n"), &parsed, nullptr));
    CHECK(parsed.luna_color == XD_LUNA_OLIVE);
    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=EnergyBlue\n"), &parsed, nullptr));
    CHECK(parsed.luna_color == XD_LUNA_ENERGY);
    CHECK(xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=Dark\n"), &parsed, nullptr));
    CHECK(parsed.luna_color == XD_LUNA_DARK);
    CHECK(!xd_scheme_parse(XD_STR("[Scheme]\nLunaColor=Green\n"), &parsed, nullptr));
    CHECK(parsed.luna_color == XD_LUNA_NONE);
}

static void test_render(void) {
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    static const double scales[] = {1.0, 1.25, 1.5, 2.0, 3.0};

    for (size_t si = 0; si < XD_STYLE_COUNT; si++) {
        for (size_t k = 0; k < XD_ARRAY_LEN(scales); k++) {
            xd_theme theme;
            xd_theme_init(&theme, (xd_visual_style)si, &scheme, scales[k]);
            int width = xd_tpx(&theme, 300);
            int height = xd_tpx(&theme, 200);
            cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
            cairo_t *cairo = cairo_create(surf);
            xd_frame frame = {.title = "Test", .active = true, .buttons = 0x0F};
            xd_theme_draw_frame(&theme, cairo, (xd_rect){0, 0, width, height}, &frame);
            xd_theme_draw_button(&theme, cairo, (xd_rect){10, 100, 80, 24}, XD_STATE_FOCUSED, "OK");
            xd_theme_draw_checkbox(&theme, cairo, (xd_rect){10, 130, 200, 20}, XD_STATE_CHECKED,
                                   "x");
            xd_theme_draw_taskbar(&theme, cairo, (xd_rect){0, height - 30, width, 30},
                                  XD_EDGE_BOTTOM);
            CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);

            /* Frame insets grow with the scale factor. */
            xd_insets in = xd_theme_frame_insets(&theme, &frame);
            CHECK(in.top >= (int)(18 * scales[k]));
            cairo_destroy(cairo);
            cairo_surface_destroy(surf);
        }
    }

    /* Classic at 1x: the outer frame pixel is ButtonLight, the next one
     * in is ButtonHilight (white), matching Win2000's EDGE_RAISED. */
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
    cairo_t *cairo = cairo_create(surf);
    xd_frame frame = {.title = "", .active = true, .buttons = 0};
    xd_theme_draw_frame(&theme, cairo, (xd_rect){0, 0, 100, 100}, &frame);
    CHECK(pixel(surf, 50, 0) == 0xD4D0C8);
    CHECK(pixel(surf, 50, 1) == 0xFFFFFF);
    CHECK(pixel(surf, 50, 99) == 0x404040);
    CHECK(pixel(surf, 50, 98) == 0x808080);
    /* Caption starts at 4px and is ActiveTitle on the left. */
    CHECK(near(pixel(surf, 4, 10), 0x0A246A, 3));
    cairo_destroy(cairo);
    cairo_surface_destroy(surf);
}

static void test_hit(void) {
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    static const double scales[] = {1.0, 1.25, 2.0};
    for (size_t si = 0; si < XD_STYLE_COUNT; si++) {
        for (size_t k = 0; k < XD_ARRAY_LEN(scales); k++) {
            xd_theme theme;
            xd_theme_init(&theme, (xd_visual_style)si, &scheme, scales[k]);
            xd_rect outer = {10, 20, xd_tpx(&theme, 300), xd_tpx(&theme, 200)};
            xd_frame frame = {.title = "x", .active = true, .buttons = 0x07};
            xd_insets in = xd_theme_frame_insets(&theme, &frame);

            CHECK(xd_theme_frame_hit(&theme, outer, &frame, 0, 0) == XD_PART_NONE);
            CHECK(xd_theme_frame_hit(&theme, outer, &frame, outer.x + outer.w / 2,
                                     outer.y + outer.h / 2) == XD_PART_CLIENT);
            CHECK(xd_theme_frame_hit(&theme, outer, &frame, outer.x, outer.y) == XD_PART_TOP_LEFT);
            CHECK(xd_theme_frame_hit(&theme, outer, &frame, outer.x + outer.w - 1,
                                     outer.y + outer.h - 1) == XD_PART_BOTTOM_RIGHT);
            CHECK(xd_theme_frame_hit(&theme, outer, &frame, outer.x + outer.w / 2,
                                     outer.y + outer.h - 1) == XD_PART_BOTTOM);
            CHECK(xd_theme_frame_hit(&theme, outer, &frame, outer.x, outer.y + outer.h / 2) ==
                  XD_PART_LEFT);
            CHECK(xd_theme_frame_hit(&theme, outer, &frame, outer.x + outer.w / 2,
                                     outer.y + in.top / 2 + xd_tpx(&theme, 2)) == XD_PART_CAPTION);

            /* Every laid-out button hit-tests to itself at its centre. */
            xd_frame_layout frame_layout;
            xd_theme_frame_layout(&theme, outer, &frame, &frame_layout);
            CHECK(frame_layout.button_count == 3);
            for (size_t i = 0; i < frame_layout.button_count; i++) {
                xd_rect rect = frame_layout.button_rects[i];
                CHECK(xd_theme_frame_hit(&theme, outer, &frame, rect.x + rect.w / 2,
                                         rect.y + rect.h / 2) == frame_layout.button_parts[i]);
                /* Buttons sit inside the caption band. */
                CHECK(rect.y >= outer.y && rect.y + rect.h <= outer.y + in.top);
            }

            /* Maximized frames have no resize borders. */
            frame.maximized = true;
            CHECK(!xd_frame_part_is_border(
                xd_theme_frame_hit(&theme, outer, &frame, outer.x, outer.y)));
        }
    }
}

/* Writes a .desktop file into a fresh temp dir and loads it via GIO. */
static bool load_entry(const char *dir, const char *name, const char *text, xd_arena *arena,
                       xd_app *out) {
    xd_strbuf path = {0};
    xd_strbuf_appendf(&path, "%s/%s", dir, name);
    FILE *file = fopen(xd_strbuf_cstr(&path), "w");
    bool ok = file != nullptr && fputs(text, file) >= 0;
    if (file != nullptr)
        fclose(file);
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
    xd_strbuf buffer = {0};
    for (size_t i = 0; i < XD_ARRAY_LEN(files); i++) {
        xd_strbuf_clear(&buffer);
        xd_strbuf_appendf(&buffer, "%s/%s", dir, files[i]);
        unlink(xd_strbuf_cstr(&buffer));
    }
    xd_strbuf_free(&buffer);
    rmdir(dir);
    xd_arena_destroy(&arena);
}

static void test_menus(void) {
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    for (size_t si = 0; si < XD_STYLE_COUNT; si++) {
        xd_theme theme;
        xd_theme_init(&theme, (xd_visual_style)si, &scheme, 1.5);
        xd_menu_metrics menu_metrics = xd_theme_menu_metrics(&theme);
        CHECK(menu_metrics.large_icon == 48 && menu_metrics.icon == 24 &&
              menu_metrics.item_h > menu_metrics.icon);
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 300);
        cairo_t *cairo = cairo_create(surf);
        xd_theme_draw_menu_frame(&theme, cairo, (xd_rect){0, 0, 300, 300});
        xd_theme_draw_start_banner(&theme, cairo, (xd_rect){4, 4, menu_metrics.banner_w, 290},
                                   "Xeon", "Pro");
        xd_theme_draw_menu_item(&theme, cairo, (xd_rect){30, 4, 260, menu_metrics.item_h},
                                XD_STATE_HOT, "A", true, false);
        xd_theme_draw_menu_item(&theme, cairo, (xd_rect){30, 40, 260, menu_metrics.item_h},
                                XD_STATE_DISABLED, "B", false, true);
        xd_theme_draw_menu_separator(&theme, cairo,
                                     (xd_rect){30, 90, 260, menu_metrics.separator_h});
        CHECK(cairo_status(cairo) == CAIRO_STATUS_SUCCESS);
        cairo_destroy(cairo);
        cairo_surface_destroy(surf);
    }
}

/* Access keys written as Windows writes them, and their underline. */
static void test_access_keys(void) {
    struct {
        const char *in, *out;
        int key;
    } cases[] = {
        {"&Restore", "Restore", 0},     {"Mi&nimize", "Minimize", 2},
        {"Ma&ximize", "Maximize", 2},   {"&Taskbar && Start Menu...", "Taskbar & Start Menu...", 0},
        {"Tom && Jerry", "Tom & Jerry", -1}, {"No key", "No key", -1},
        {"Trailing&", "Trailing", -1},  {"&a&b", "ab", 0},
        {"Caf\xC3\xA9 &\xC3\x89t\xC3\xA9", "Caf\xC3\xA9 \xC3\x89t\xC3\xA9", 6},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        int key = -2;
        char *out = xd_text_strip_prefix(cases[i].in, &key);
        CHECK(strcmp(out, cases[i].out) == 0);
        CHECK(key == cases[i].key);
        g_free(out);
    }

    /* The underline shows under the letter, and only there. */
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    xd_menu_metrics menu_metrics = xd_theme_menu_metrics(&theme);
    cairo_surface_t *surf[2];
    static const char label[] = "Minimize";
    for (int underlined = 0; underlined < 2; underlined++) {
        surf[underlined] =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 160, menu_metrics.item_h);
        cairo_t *cairo = cairo_create(surf[underlined]);
        xd_theme_draw_menu_frame(&theme, cairo, (xd_rect){0, 0, 160, menu_metrics.item_h});
        xd_menu_item_extras menu_item_extras = {.underline = underlined == 1 ? label + 2 : nullptr};
        xd_theme_draw_menu_item_ex(&theme, cairo, (xd_rect){0, 0, 160, menu_metrics.item_h}, 0,
                                   label, false, false, &menu_item_extras);
        cairo_destroy(cairo);
        cairo_surface_flush(surf[underlined]);
    }
    int stride = cairo_image_surface_get_stride(surf[0]);
    const uint8_t *plain_data = cairo_image_surface_get_data(surf[0]);
    const uint8_t *underlined_data = cairo_image_surface_get_data(surf[1]);
    int changed = 0, left = 160, right = -1;
    for (int y = 0; y < menu_metrics.item_h; y++) {
        for (int x = 0; x < 160; x++) {
            if (memcmp(plain_data + y * stride + x * 4, underlined_data + y * stride + x * 4, 4) !=
                0) {
                changed++;
                left = XD_MIN(left, x);
                right = XD_MAX(right, x);
            }
        }
    }
    /* A short line under the "n", right of the text's start. */
    CHECK(changed > 0 && right - left < 12 && left > menu_metrics.text_x);
    cairo_surface_destroy(surf[0]);
    cairo_surface_destroy(surf[1]);
}

static bool text_is(const xd_textedit *edit, const char *want) {
    return xd_str_eq(xd_textedit_view(edit), xd_str_from_cstr(want));
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

    xd_textedit edit = {0};
    xd_textedit_set(&edit, XD_STR("hello"));
    CHECK(text_is(&edit, "hello") && xd_textedit_has_selection(&edit));
    xd_textedit_insert(&edit, XD_STR("x")); /* typing replaces the selection */
    CHECK(text_is(&edit, "x") && edit.cursor == 1);

    /* Control characters and invalid bytes are dropped. */
    xd_textedit_insert(&edit, XD_STR("a\tb\n\x01\xFF\xC3\xA9"));
    CHECK(text_is(&edit, "xab\xC3\xA9"));
    /* Backspace removes a whole character, not a byte. */
    xd_textedit_backspace(&edit, false);
    CHECK(text_is(&edit, "xab"));

    /* Cursor never lands inside a character. */
    xd_textedit_set(&edit, XD_STR("\xE2\x82\xAC"
                                  "1"));
    xd_textedit_set_cursor(&edit, 2, false);
    CHECK(edit.cursor == 0);
    xd_textedit_move(&edit, XD_MOTION_RIGHT, false);
    CHECK(edit.cursor == 3);
    xd_textedit_delete(&edit, false);
    CHECK(text_is(&edit, "\xE2\x82\xAC"));

    /* Selection, word motion, word delete. */
    xd_textedit_set(&edit, XD_STR("ls -la /tmp"));
    xd_textedit_move(&edit, XD_MOTION_END, false);
    xd_textedit_move(&edit, XD_MOTION_WORD_LEFT, true);
    CHECK(xd_str_eq(xd_textedit_selected(&edit), XD_STR("tmp")));
    xd_textedit_move(&edit, XD_MOTION_LEFT, false); /* collapses to the left edge */
    CHECK(edit.cursor == 8 && !xd_textedit_has_selection(&edit));
    xd_textedit_move(&edit, XD_MOTION_HOME, false);
    xd_textedit_move(&edit, XD_MOTION_WORD_RIGHT, false);
    CHECK(edit.cursor == 4);
    xd_textedit_move(&edit, XD_MOTION_END, false);
    xd_textedit_backspace(&edit, true);
    CHECK(text_is(&edit, "ls -la /"));
    xd_textedit_move(&edit, XD_MOTION_HOME, false);
    xd_textedit_delete(&edit, true);
    CHECK(text_is(&edit, "la /"));

    /* Insertion in the middle; max length cut on a character boundary. */
    xd_textedit_set(&edit, XD_STR("ac"));
    xd_textedit_set_cursor(&edit, 1, false);
    xd_textedit_insert(&edit, XD_STR("b"));
    CHECK(text_is(&edit, "abc") && edit.cursor == 2);
    edit.max_len = 5;
    xd_textedit_move(&edit, XD_MOTION_END, false);
    xd_textedit_insert(&edit, XD_STR("\xC3\xA9\xC3\xA9")); /* 4 bytes, room for 2 */
    CHECK(text_is(&edit, "abc\xC3\xA9"));
    xd_textedit_free(&edit);
}

static void test_runcmd(void) {
    const char *path = "/usr/bin:/bin";
    xd_run_target run_target = xd_run_classify(XD_STR("   "), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_EMPTY);
    xd_run_target_free(&run_target);

    run_target = xd_run_classify(XD_STR("https://example.com/x"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_OPEN);
    xd_run_target_free(&run_target);
    run_target = xd_run_classify(XD_STR("www.example.com"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_OPEN);
    xd_run_target_free(&run_target);
    run_target = xd_run_classify(XD_STR("mailto:a@b.c"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_OPEN);
    xd_run_target_free(&run_target);

    run_target = xd_run_classify(XD_STR("/tmp"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_OPEN &&
          strcmp(xd_strbuf_cstr(&run_target.target), "/tmp") == 0);
    xd_run_target_free(&run_target);
    run_target = xd_run_classify(XD_STR("~"), path, "/tmp");
    CHECK(run_target.kind == XD_RUN_OPEN &&
          strcmp(xd_strbuf_cstr(&run_target.target), "/tmp") == 0);
    xd_run_target_free(&run_target);
    run_target = xd_run_classify(XD_STR("/bin/sh"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_EXEC);
    xd_run_target_free(&run_target);

    run_target = xd_run_classify(XD_STR("sh -c 'echo hi'"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_SHELL &&
          strcmp(xd_strbuf_cstr(&run_target.target), "sh -c 'echo hi'") == 0);
    xd_run_target_free(&run_target);
    run_target = xd_run_classify(XD_STR("/bin/sh -c true"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_SHELL);
    xd_run_target_free(&run_target);

    run_target = xd_run_classify(XD_STR("no-such-program-xd --x"), path, "/home/u");
    CHECK(run_target.kind == XD_RUN_NOT_FOUND &&
          strcmp(xd_strbuf_cstr(&run_target.target), "no-such-program-xd") == 0);
    xd_run_target_free(&run_target);
    /* "sh" isn't found in an empty PATH: nothing runs from the cwd. */
    run_target = xd_run_classify(XD_STR("sh"), "", "/home/u");
    CHECK(run_target.kind == XD_RUN_NOT_FOUND);
    xd_run_target_free(&run_target);
}

static void test_tray_rules(void) {
    xd_settings original;
    xd_settings_parse(XD_STR("[Notifications]\nnm-applet=hide\nsteam=show\nbad=whatever\n"
                             "quiet=auto\n"),
                      &original);
    CHECK(xd_settings_tray_rule(&original, "nm-applet") == XD_TRAY_ALWAYS_HIDE);
    CHECK(xd_settings_tray_rule(&original, "steam") == XD_TRAY_ALWAYS_SHOW);
    CHECK(xd_settings_tray_rule(&original, "bad") == XD_TRAY_AUTO);
    CHECK(xd_settings_tray_rule(&original, "quiet") == XD_TRAY_AUTO &&
          original.tray_rule_count == 2);
    xd_settings_set_tray_rule(&original, "steam", XD_TRAY_AUTO);
    CHECK(xd_settings_tray_rule(&original, "steam") == XD_TRAY_AUTO &&
          original.tray_rule_count == 1);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          xd_settings_tray_rule(&parsed, "nm-applet") == XD_TRAY_ALWAYS_HIDE);
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
    GDir *folder_handle = g_dir_open(path, 0, nullptr);
    if (folder_handle != nullptr) {
        const char *file_name;
        while ((file_name = g_dir_read_name(folder_handle)) != nullptr) {
            char *child = g_build_filename(path, file_name, nullptr);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
                !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                rm_tree(child);
            else
                CHECK(g_remove(child) == 0);
            g_free(child);
        }
        g_dir_close(folder_handle);
    }
    CHECK(g_rmdir(path) == 0);
}

#if XD_HAVE_LIBARCHIVE
/* Writes a tar.gz of entries (path, type, mode, contents or link). */
typedef struct tar_item {
    const char *path;
    mode_t type;
    int perm;
    const char *data; /* a symlink's or hard link's target */
} tar_item;

static void write_tar(const char *file, const tar_item *items, size_t count) {
    struct archive *writer = archive_write_new();
    archive_write_add_filter_gzip(writer);
    archive_write_set_format_pax_restricted(writer);
    CHECK(archive_write_open_filename(writer, file) == ARCHIVE_OK);
    for (size_t i = 0; i < count; i++) {
        struct archive_entry *member = archive_entry_new();
        archive_entry_set_pathname(member, items[i].path);
        archive_entry_set_perm(member, (mode_t)items[i].perm);
        if (items[i].type == 0) { /* a hard link */
            archive_entry_set_filetype(member, AE_IFREG);
            archive_entry_set_hardlink(member, items[i].data);
        } else {
            archive_entry_set_filetype(member, items[i].type);
        }
        if (items[i].type == AE_IFLNK)
            archive_entry_set_symlink(member, items[i].data);
        size_t len = items[i].type == AE_IFREG && items[i].data != nullptr
                         ? strlen(items[i].data)
                         : 0;
        archive_entry_set_size(member, (la_int64_t)len);
        CHECK(archive_write_header(writer, member) == ARCHIVE_OK);
        if (len > 0)
            CHECK(archive_write_data(writer, items[i].data, len) == (la_ssize_t)len);
        archive_entry_free(member);
    }
    CHECK(archive_write_close(writer) == ARCHIVE_OK);
    archive_write_free(writer);
}

static char *read_text(const char *dir, const char *name) {
    char *path = g_build_filename(dir, name, nullptr);
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        text = nullptr;
    g_free(path);
    return text;
}
#endif

/* Extract Here / Extract to "name" (src/files/extract.c). */
static void test_extract(void) {
    char *base_name = xd_archive_base_name("photos.tar.gz");
    CHECK(strcmp(base_name, "photos") == 0);
    g_free(base_name);
    const char *const names[][2] = {
        {"photos.TGZ", "photos"},
        {"photos.zip", "photos"},
        {"notes.txt.gz", "notes.txt"},
        {"old.Z", "old"},
        {"site.tar.zst", "site"},
        {"backup.7z", "backup"},
        {"disk.iso", "disk"},
        {"weird.foo", "weird"},
        {"noext", "noext (extracted)"},
        {"pkg-1.0.tar.xz", "pkg-1.0"},
        /* Never the folder above, or this one. */
        {"...zip", "...zip (extracted)"},
        {"..zip", "..zip (extracted)"},
        {"..tar.gz", "..tar.gz (extracted)"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        char *got = xd_archive_base_name(names[i][0]);
        if (strcmp(got, names[i][1]) != 0)
            g_printerr("  %s: got \"%s\"\n", names[i][0], got);
        CHECK(strcmp(got, names[i][1]) == 0);
        g_free(got);
    }
    const char *const paths[][2] = {
        {"a/b.txt", "a/b.txt"}, {"./a//b/", "a/b"}, {"/etc/passwd", "etc/passwd"},
        {"../x", nullptr},      {"a/../../x", nullptr}, {".", nullptr},
        {"", nullptr},          {"a/..b", "a/..b"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(paths); i++) {
        char *got = xd_archive_safe_path(paths[i][0]);
        CHECK(g_strcmp0(got, paths[i][1]) == 0);
        g_free(got);
    }
    CHECK(xd_is_archive_type("application/zip") && xd_is_archive_type("application/x-compressed-tar"));
    CHECK(xd_is_archive_type("application/x-zip-compressed")); /* an alias */
    CHECK(!xd_is_archive_type("application/vnd.oasis.opendocument.text") &&
          !xd_is_archive_type("text/plain") && !xd_is_archive_type(nullptr));

#if XD_HAVE_LIBARCHIVE
    char *root = g_dir_make_tmp("xd-extract-XXXXXX", nullptr);
    char *into = g_build_filename(root, "into", nullptr);
    char *outside = g_build_filename(root, "outside", nullptr);
    CHECK(g_mkdir(into, 0755) == 0 && g_mkdir(outside, 0755) == 0);

    /* What lands outside, a device-like entry and set-user-ID are left out. */
    char *tar = g_build_filename(root, "t.tar.gz", nullptr);
    const tar_item items[] = {
        {"./dir", AE_IFDIR, 0755, nullptr},
        {"dir/a.txt", AE_IFREG, 0644, "a"},
        {"../evil.txt", AE_IFREG, 0644, "evil"},
        {"/abs.txt", AE_IFREG, 0644, "abs"},
        {"fifo", AE_IFIFO, 0644, nullptr},
        {"suid", AE_IFREG, 04755, "s"},
        {"hl", 0, 0644, "../outside/x"},
        {"same", 0, 0644, "dir/a.txt"},
        {"up", AE_IFLNK, 0777, "../outside"},
    };
    write_tar(tar, items, G_N_ELEMENTS(items));
    GError *error = nullptr;
    CHECK(xd_extract_archive(tar, into, "raw", nullptr, nullptr, nullptr, &error));
    if (error != nullptr)
        g_printerr("  %s\n", error->message);
    g_clear_error(&error);
    char *text = read_text(into, "dir/a.txt");
    CHECK(g_strcmp0(text, "a") == 0);
    g_free(text);
    text = read_text(into, "abs.txt");
    CHECK(g_strcmp0(text, "abs") == 0);
    g_free(text);
    text = read_text(into, "same");
    CHECK(g_strcmp0(text, "a") == 0); /* a hard link inside */
    g_free(text);
    char *evil = g_build_filename(root, "evil.txt", nullptr);
    char *fifo = g_build_filename(into, "fifo", nullptr);
    char *hl = g_build_filename(into, "hl", nullptr);
    char *suid = g_build_filename(into, "suid", nullptr);
    struct stat st;
    CHECK(!g_file_test(evil, G_FILE_TEST_EXISTS) && !g_file_test(fifo, G_FILE_TEST_EXISTS) &&
          !g_file_test(hl, G_FILE_TEST_EXISTS));
    CHECK(stat(suid, &st) == 0 && (st.st_mode & 07777) == 0755);

    /* Nothing written through a link the archive made: "up/x.txt" after
     * "up" -> ../outside stops the extraction. */
    char *tar2 = g_build_filename(root, "t2.tar.gz", nullptr);
    char *into2 = g_build_filename(root, "into2", nullptr);
    CHECK(g_mkdir(into2, 0755) == 0);
    const tar_item items2[] = {
        {"up", AE_IFLNK, 0777, "../outside"},
        {"up/x.txt", AE_IFREG, 0644, "x"},
    };
    write_tar(tar2, items2, G_N_ELEMENTS(items2));
    CHECK(!xd_extract_archive(tar2, into2, "raw", nullptr, nullptr, nullptr, &error));
    g_clear_error(&error);
    char *escaped_path = g_build_filename(outside, "x.txt", nullptr);
    CHECK(!g_file_test(escaped_path, G_FILE_TEST_EXISTS));

    /* A gzipped file that's no archive: its contents, under the name. */
    char *gz = g_build_filename(root, "notes.txt.gz", nullptr);
    struct archive *writer = archive_write_new();
    archive_write_add_filter_gzip(writer);
    archive_write_set_format_raw(writer);
    CHECK(archive_write_open_filename(writer, gz) == ARCHIVE_OK);
    struct archive_entry *member = archive_entry_new();
    archive_entry_set_filetype(member, AE_IFREG);
    CHECK(archive_write_header(writer, member) == ARCHIVE_OK);
    CHECK(archive_write_data(writer, "hello", 5) == 5);
    archive_entry_free(member);
    CHECK(archive_write_close(writer) == ARCHIVE_OK);
    archive_write_free(writer);
    char *into3 = g_build_filename(root, "into3", nullptr);
    CHECK(g_mkdir(into3, 0755) == 0);
    CHECK(xd_extract_archive(gz, into3, "notes.txt", nullptr, nullptr, nullptr, nullptr));
    text = read_text(into3, "notes.txt");
    CHECK(g_strcmp0(text, "hello") == 0);
    g_free(text);

    rm_tree(root);
    g_free(into3);
    g_free(gz);
    g_free(escaped_path);
    g_free(into2);
    g_free(tar2);
    g_free(suid);
    g_free(hl);
    g_free(fifo);
    g_free(evil);
    g_free(tar);
    g_free(outside);
    g_free(into);
    g_free(root);
#endif
}

/* ---- Theme image scaling (src/theme/scale.c) ---- */

/* An ARGB32 surface from w*h premultiplied pixels, row by row. */
static cairo_surface_t *scale_test_surface(int width, int height, const uint32_t *px) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < height; y++)
        memcpy(data + (size_t)y * (size_t)stride, px + (size_t)y * (size_t)width,
               (size_t)width * sizeof(uint32_t));
    cairo_surface_mark_dirty(surface);
    return surface;
}

static uint32_t scale_test_px(cairo_surface_t *surface, int x, int y) {
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    uint32_t value;
    memcpy(&value,
           data + (size_t)y * (size_t)cairo_image_surface_get_stride(surface) + (size_t)x * 4, 4);
    return value;
}

/* Every pixel of `surface` (w x h) equals `want`, row by row. */
static bool scale_test_equals(cairo_surface_t *surface, int width, int height,
                              const uint32_t *want) {
    if (!surface || cairo_image_surface_get_width(surface) != width ||
        cairo_image_surface_get_height(surface) != height)
        return false;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (scale_test_px(surface, x, y) != want[y * width + x])
                return false;
    return true;
}

static bool scale_test_same(cairo_surface_t *left, cairo_surface_t *right) {
    int width = cairo_image_surface_get_width(left), height = cairo_image_surface_get_height(left);
    if (width != cairo_image_surface_get_width(right) ||
        height != cairo_image_surface_get_height(right))
        return false;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            if (scale_test_px(left, x, y) != scale_test_px(right, x, y))
                return false;
    return true;
}

static void test_image_scale(void) {
    constexpr uint32_t black = 0xff000000;
    constexpr uint32_t white = 0xffffffff;
    constexpr uint32_t pure_red = 0xffff0000;
    constexpr uint32_t blue = 0xff0000ff;
    constexpr uint32_t gray = 0xff808080; /* K and W half and half */

    /* Names round-trip, case-insensitively. */
    for (int filter_index = 0; filter_index < XD_FILTER_COUNT; filter_index++) {
        xd_image_filter got = XD_FILTER_COUNT;
        const char *name = xd_image_filter_name((xd_image_filter)filter_index);
        CHECK(name && xd_image_filter_from_name(name, &got) &&
              got == (xd_image_filter)filter_index);
    }
    xd_image_filter got = XD_FILTER_NEAREST;
    CHECK(xd_image_filter_from_name("superEAGLE", &got) && got == XD_FILTER_SUPEREAGLE);
    CHECK(xd_image_filter_from_name("2xsai", &got) && got == XD_FILTER_2XSAI);
    CHECK(!xd_image_filter_from_name("Bicubic", &got));
    CHECK(!xd_image_filter_from_name(nullptr, &got));
    CHECK(xd_image_filter_name(XD_FILTER_COUNT) == nullptr);

    /* Sizes: the source's times the scale, rounded, for every filter. */
    {
        uint32_t px[8 * 6];
        for (int i = 0; i < 8 * 6; i++)
            px[i] = (i * 7) % 3 == 0 ? pure_red : (i % 5 == 0 ? 0 : blue);
        cairo_surface_t *src = scale_test_surface(8, 6, px);
        const double scales[] = {0.5, 1.25, 1.5, 2, 3, 4};
        for (int filter_index = 0; filter_index < XD_FILTER_COUNT; filter_index++) {
            for (size_t k = 0; k < G_N_ELEMENTS(scales); k++) {
                cairo_surface_t *surface =
                    xd_image_scale(src, scales[k], (xd_image_filter)filter_index);
                CHECK(surface && cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32);
                double want_w = round(8 * scales[k]), want_h = round(6 * scales[k]);
                CHECK(surface && cairo_image_surface_get_width(surface) == (int)want_w);
                CHECK(surface && cairo_image_surface_get_height(surface) == (int)want_h);
                if (surface)
                    cairo_surface_destroy(surface);
            }
            /* A scale of 1 copies. */
            cairo_surface_t *surface = xd_image_scale(src, 1, (xd_image_filter)filter_index);
            CHECK(scale_test_equals(surface, 8, 6, px));
            if (surface)
                cairo_surface_destroy(surface);
        }
        CHECK(xd_image_scale(src, 0, XD_FILTER_SMOOTH) == nullptr);
        CHECK(xd_image_scale(src, -2, XD_FILTER_SMOOTH) == nullptr);
        CHECK(xd_image_scale(src, 2, XD_FILTER_COUNT) == nullptr);
        CHECK(xd_image_scale(nullptr, 2, XD_FILTER_SMOOTH) == nullptr);
        cairo_surface_destroy(src);
    }

    /* NEAREST: a 2x2 checker; at 1.5 (3x3) each output pixel takes the
     * source pixel under its centre (0.33, 1.0, 1.67 -> 0, 1, 1). */
    {
        const uint32_t px[] = {black, white, white, black};
        cairo_surface_t *src = scale_test_surface(2, 2, px);
        cairo_surface_t *surface = xd_image_scale(src, 1.5, XD_FILTER_NEAREST);
        const uint32_t want15[] = {black, white, white, white, black, black, white, black, black};
        CHECK(scale_test_equals(surface, 3, 3, want15));
        cairo_surface_destroy(surface);
        surface = xd_image_scale(src, 2, XD_FILTER_NEAREST);
        const uint32_t want2[] = {black, black, white, white, black, black, white, white,
                                  white, white, black, black, white, white, black, black};
        CHECK(scale_test_equals(surface, 4, 4, want2));
        cairo_surface_destroy(surface);
        /* And nearest stays nearest going down: 2x2 -> 1x1 takes (1,1). */
        surface = xd_image_scale(src, 0.5, XD_FILTER_NEAREST);
        const uint32_t want05[] = {black};
        CHECK(scale_test_equals(surface, 1, 1, want05));
        cairo_surface_destroy(surface);
        cairo_surface_destroy(src);
    }

    /* INTEGER at 2 and 3: exact blocks of a 3x3 of distinct colours. */
    {
        uint32_t px[9];
        for (uint32_t i = 0; i < 9; i++)
            px[i] = 0xff000000 | (i * 0x1d0b07);
        cairo_surface_t *src = scale_test_surface(3, 3, px);
        for (int count = 2; count <= 3; count++) {
            cairo_surface_t *surface = xd_image_scale(src, count, XD_FILTER_INTEGER);
            bool ok = surface && cairo_image_surface_get_width(surface) == 3 * count &&
                      cairo_image_surface_get_height(surface) == 3 * count;
            for (int y = 0; ok && y < 3 * count; y++)
                for (int x = 0; x < 3 * count; x++)
                    ok = ok && scale_test_px(surface, x, y) == px[(y / count) * 3 + x / count];
            CHECK(ok);
            cairo_surface_destroy(surface);
        }
        cairo_surface_destroy(src);
    }

    /* A diagonal edge, black below-left of it:
     *     K W W
     *     K K W
     *     K K K                                                           */
    const uint32_t tri[] = {black, white, white, black, black, white, black, black, black};
    cairo_surface_t *tri_src = scale_test_surface(3, 3, tri);

    /* Scale2x: where a pixel's two outer neighbours agree (and the other
     * two don't continue the edge), its quarter between them takes their
     * colour. W at (1,0) has K left and below: its bottom-left quarter,
     * output (2,1), turns K; K at (1,1) has W above and right: its
     * top-right quarter, (3,2), turns W. Same for (2,1) -> (4,3) K, and
     * K at (2,2), whose D == F, stays whole. */
    {
        cairo_surface_t *surface = xd_image_scale(tri_src, 2, XD_FILTER_SCALE2X);
        const uint32_t want[] = {
            black, black, white, white, white, white, //
            black, black, black, white, white, white, //
            black, black, black, white, white, white, //
            black, black, black, black, black, white, //
            black, black, black, black, black, black, //
            black, black, black, black, black, black, //
        };
        CHECK(scale_test_equals(surface, 6, 6, want));
        cairo_surface_destroy(surface);
    }
    /* Scale3x: W at (1,0) (A=K B=W C=W / D=K E=W F=W / G=K H=K I=W):
     * corner 6 (D == H) and edge 3 (D == H, E != A) become K, the rest W:
     *     W W W
     *     K W W
     *     K W W                                                           */
    {
        cairo_surface_t *surface = xd_image_scale(tri_src, 3, XD_FILTER_SCALE3X);
        const uint32_t block[] = {white, white, white, black, white, white, black, white, white};
        bool ok = surface && cairo_image_surface_get_width(surface) == 9;
        for (int k = 0; ok && k < 9; k++)
            ok = scale_test_px(surface, 3 + k % 3, k / 3) == block[k];
        /* (0,0) K and (2,0) W are inside flat areas: unchanged blocks. */
        for (int k = 0; ok && k < 9; k++)
            ok = scale_test_px(surface, k % 3, k / 3) == black &&
                 scale_test_px(surface, 6 + k % 3, k / 3) == white;
        CHECK(ok);
        cairo_surface_destroy(surface);
    }

    /* The SaI family: deterministic, and not nearest on the diagonal.
     * Hand-checked pixels, with the 4x4 neighbourhood named
     *     i e f j / g a b k / h c d l / m n o p
     * around the square whose top-left is a:
     *
     * 2xSaI, a = (1,0) = W: b = W, c = K, d = W; e = W, l = W, g = K, o = K.
     *   a == d and b != c: a diagonal of a's colour. The right point is a
     *   (a == e and b == l); the point below is neither of its exceptions
     *   (a != g; a != h), so it's the half blend of a and c: output (2,1)
     *   = G, where nearest gives W.
     * Super2xSaI, square (1,0): a = W, b = W, c = K, d = W, g = K, h = K.
     *   a == d, b != c: the right column is a, so output (3,2) (bottom
     *   right of the square, rows 1-2) is W, where nearest gives K.
     * SuperEagle, square (1,0), same pixels, o = K: a == d, b != c, so
     *   corner c (output (3,2)) is cut by the diagonal; neither g nor o
     *   continues it (g != a, o != d), so it's half d, half c: G.        */
    {
        const struct {
            xd_image_filter f;
            int x, y;
            uint32_t want;
        } sai[] = {
            {XD_FILTER_2XSAI, 2, 1, gray},
            {XD_FILTER_SUPER2XSAI, 3, 2, white},
            {XD_FILTER_SUPEREAGLE, 3, 2, gray},
        };
        cairo_surface_t *near = xd_image_scale(tri_src, 2, XD_FILTER_NEAREST);
        for (size_t k = 0; k < G_N_ELEMENTS(sai); k++) {
            cairo_surface_t *o1 = xd_image_scale(tri_src, 2, sai[k].f);
            cairo_surface_t *o2 = xd_image_scale(tri_src, 2, sai[k].f);
            CHECK(o1 && o2 && scale_test_same(o1, o2));
            CHECK(o1 && !scale_test_same(o1, near));
            CHECK(o1 && scale_test_px(o1, sai[k].x, sai[k].y) == sai[k].want);
            CHECK(scale_test_px(near, sai[k].x, sai[k].y) != sai[k].want);
            cairo_surface_destroy(o1);
            cairo_surface_destroy(o2);
        }
        cairo_surface_destroy(near);
    }
    cairo_surface_destroy(tri_src);

    /* Transparency: half-transparent red with transparent holes and
     * surroundings. No pixel-art filter (nor the smooth ones) may invent
     * another colour, push alpha above the inputs' 0x80, or let colour
     * exceed alpha. */
    {
        constexpr uint32_t half_red = 0x80800000;
        const uint32_t px[] = {
            0, 0,        0,        0,        0, //
            0, half_red, half_red, half_red, 0, //
            0, half_red, 0,        half_red, 0, //
            0, half_red, half_red, 0,        0, //
            0, 0,        0,        0,        0, //
        };
        cairo_surface_t *src = scale_test_surface(5, 5, px);
        const double scales[] = {1.5, 2, 3, 4};
        for (int filter_index = 0; filter_index < XD_FILTER_COUNT; filter_index++) {
            for (size_t k = 0; k < G_N_ELEMENTS(scales); k++) {
                cairo_surface_t *surface =
                    xd_image_scale(src, scales[k], (xd_image_filter)filter_index);
                bool ok = surface != nullptr;
                int width = surface ? cairo_image_surface_get_width(surface) : 0;
                int height = surface ? cairo_image_surface_get_height(surface) : 0;
                for (int y = 0; ok && y < height; y++) {
                    for (int x = 0; ok && x < width; x++) {
                        uint32_t value = scale_test_px(surface, x, y);
                        uint32_t alpha = value >> 24, red = (value >> 16) & 0xff;
                        ok = alpha <= 0x80 && red <= alpha && (value & 0xffff) == 0;
                    }
                }
                CHECK(ok);
                if (surface)
                    cairo_surface_destroy(surface);
            }
        }
        cairo_surface_destroy(src);
    }

    /* scale_frames: two very different frames keep their own colours,
     * however the filter reads its neighbours. */
    {
        uint32_t px[4 * 8];
        for (int i = 0; i < 4 * 8; i++)
            px[i] = i < 16 ? pure_red : blue;
        cairo_surface_t *vsrc = scale_test_surface(4, 8, px);
        uint32_t hpx[8 * 4];
        for (int i = 0; i < 8 * 4; i++)
            hpx[i] = i % 8 < 4 ? pure_red : blue;
        cairo_surface_t *hsrc = scale_test_surface(8, 4, hpx);
        const double scales[] = {1.25, 1.5, 2, 3};
        for (int filter_index = 0; filter_index < XD_FILTER_COUNT; filter_index++) {
            for (size_t k = 0; k < G_N_ELEMENTS(scales); k++) {
                double side = round(4 * scales[k]);
                int count = (int)side;
                cairo_surface_t *surface =
                    xd_image_scale_frames(vsrc, 2, true, scales[k], (xd_image_filter)filter_index);
                bool ok = surface && cairo_image_surface_get_width(surface) == count &&
                          cairo_image_surface_get_height(surface) == 2 * count;
                for (int y = 0; ok && y < 2 * count; y++)
                    for (int x = 0; ok && x < count; x++)
                        ok = scale_test_px(surface, x, y) == (y < count ? pure_red : blue);
                CHECK(ok);
                if (surface)
                    cairo_surface_destroy(surface);
                surface =
                    xd_image_scale_frames(hsrc, 2, false, scales[k], (xd_image_filter)filter_index);
                ok = surface && cairo_image_surface_get_width(surface) == 2 * count &&
                     cairo_image_surface_get_height(surface) == count;
                for (int y = 0; ok && y < count; y++)
                    for (int x = 0; ok && x < 2 * count; x++)
                        ok = scale_test_px(surface, x, y) == (x < count ? pure_red : blue);
                CHECK(ok);
                if (surface)
                    cairo_surface_destroy(surface);
            }
        }
        /* Whereas scaling the strip whole blends across the seam. */
        cairo_surface_t *surface = xd_image_scale(vsrc, 1.5, XD_FILTER_SMOOTH);
        CHECK(surface && scale_test_px(surface, 0, 5) != pure_red &&
              scale_test_px(surface, 0, 5) != blue);
        cairo_surface_destroy(surface);
        /* A strip that doesn't divide into the frames is refused. */
        CHECK(xd_image_scale_frames(vsrc, 3, true, 2, XD_FILTER_NEAREST) == nullptr);
        cairo_surface_destroy(hsrc);
        cairo_surface_destroy(vsrc);
    }
}

static const xd_installed_theme *theme_at(GPtrArray *list, guint i) {
    return i < list->len ? g_ptr_array_index(list, i) : nullptr;
}

/* ---- theme packages (src/theme/pkg.c, style_pkg.c, controls.c) ----
 * A package made here, with images drawn here: never Microsoft's. */

/* images/NAME: w x h opaque pixels (0xRRGGBB), row by row. */
static void pkg_png(const char *root, const char *name, int width, int height, const uint32_t *px) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            uint32_t value = 0xFF000000u | px[y * width + x];
            memcpy(data + (size_t)y * (size_t)stride + (size_t)x * 4, &value, sizeof(value));
        }
    cairo_surface_mark_dirty(surface);
    char *path = g_build_filename(root, "images", name, nullptr);
    CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
    g_free(path);
    cairo_surface_destroy(surface);
}

static void pkg_png_solid(const char *root, const char *name, int width, int height, uint32_t rgb) {
    uint32_t *px = g_new(uint32_t, (gsize)(width * height));
    for (int i = 0; i < width * height; i++)
        px[i] = rgb;
    pkg_png(root, name, width, height, px);
    g_free(px);
}

static void pkg_text(const char *root, const char *name, const char *text) {
    char *path = g_build_filename(root, name, nullptr);
    CHECK(g_file_set_contents(path, text, -1, nullptr));
    g_free(path);
}

static const char *const PKG_FILES[] = {
    "theme.xml",         "styles/a.xml",     "styles/b.xml",      "images/grid.png",
    "images/states.png", "images/small.png", "images/dpi13.png",  "images/dpi26.png",
    "images/size8.png",  "images/size16.png", "images/glyph7.png", "images/glyph11.png",
    "images/atlas.png",  "images/checker.png", "images/bar.png",   "images/chunk.png",
    "images/hstrip.png",
};

static void make_test_package(const char *root) {
    char *styles = g_build_filename(root, "styles", nullptr);
    char *images = g_build_filename(root, "images", nullptr);
    CHECK(g_mkdir(styles, 0755) == 0 && g_mkdir(images, 0755) == 0);
    g_free(styles);
    g_free(images);
    pkg_text(root, "theme.xml",
             "<xdtheme version=\"1\" name=\"Test Style\" base=\"whistler\">\n"
             "  <variant id=\"a\" name=\"A\" size=\"normal\" style=\"styles/a.xml\"/>\n"
             "  <variant id=\"b\" name=\"B\" size=\"normal\" style=\"styles/b.xml\"/>\n"
             "  <scaling filter=\"Integer\"/>\n"
             "</xdtheme>\n");
    pkg_text(root, "styles/b.xml",
             "<style><class name=\"Button\"><part name=\"PushButton\" Lvl=\"b\"/></class></style>");
    pkg_text(
        root, "styles/a.xml",
        "<style>\n"
        "  <sysmetrics>\n"
        "    <color name=\"BtnFace\" value=\"1 2 3\"/>\n"
        "    <color name=\"Window\" value=\"#102030\"/>\n"
        "    <font name=\"CaptionFont\" face=\"Tahoma\" size=\"11\" bold=\"true\"/>\n"
        "    <font name=\"MenuFont\" face=\"Comic Neue\" size=\"9\"/>\n"
        "    <value name=\"CaptionBarHeight\" value=\"27\"/>\n"
        "    <value name=\"ScrollbarWidth\" value=\"19\"/>\n"
        "  </sysmetrics>\n"
        "  <class name=\"globals\"><part Glob=\"g\"/></class>\n"
        "  <class name=\"ButtonStyle\">\n"
        "    <part Lvl=\"classstyle\"/>\n"
        "    <part name=\"PushButton\" StyleProp=\"s\"/>\n"
        "    <part name=\"UserButton\" StyleProp=\"u\"/>\n"
        "  </class>\n"
        "  <class name=\"Button\">\n"
        "    <part Lvl=\"class\"/>\n"
        "    <part name=\"PushButton\" Lvl=\"part\" PartOnly=\"p\" ImageFile=\"images/states.png\"\n"
        "          ImageCount=\"3\" ImageLayout=\"Vertical\" SizingType=\"TrueSize\"/>\n"
        "    <part name=\"PushButton\" state=\"Hot\" Lvl=\"state\"/>\n"
        "    <part name=\"3\" Lvl=\"checkbox\"/>\n"
        "  </class>\n"
        "  <class name=\"Dialog::Button\"><part name=\"PushButton\" state=\"2\" Lvl=\"app\"/></class>\n"
        "  <class name=\"Toolbar\"><part state=\"Hot\" TextColor=\"1 2 3\"/></class>\n"
        "  <class name=\"Draw\">\n"
        "    <part name=\"Grid\" ImageFile=\"images/grid.png\" SizingMargins=\"4 4 4 4\"/>\n"
        "    <part name=\"Small\" ImageFile=\"images/small.png\" SizingType=\"TrueSize\"/>\n"
        "    <part name=\"States\" ImageFile=\"images/states.png\" ImageCount=\"3\"\n"
        "          ImageLayout=\"vertical\" SizingType=\"TrueSize\"/>\n"
        "    <part name=\"Strip\" ImageFile=\"images/hstrip.png\" ImageCount=\"2\"\n"
        "          SizingType=\"TrueSize\"/>\n"
        "    <part name=\"Dpi\" ImageSelectType=\"Dpi\" SizingType=\"TrueSize\"\n"
        "          ImageFile1=\"images/dpi13.png\" MinDpi1=\"96\"\n"
        "          ImageFile2=\"images/dpi26.png\" MinDpi2=\"192\"/>\n"
        "    <part name=\"BySize\" ImageSelectType=\"Size\" SizingType=\"TrueSize\"\n"
        "          ImageFile1=\"images/size8.png\" MinSize1=\"8 8\"\n"
        "          ImageFile2=\"images/size16.png\" MinSize2=\"16 16\"/>\n"
        "    <part name=\"Glyph\" BgType=\"None\" GlyphType=\"ImageGlyph\"\n"
        "          ImageFile1=\"images/glyph7.png\" ImageFile2=\"images/glyph11.png\"/>\n"
        "    <part name=\"Border\" BgType=\"BorderFill\" BorderSize=\"2\" BorderColor=\"255 0 0\"\n"
        "          FillColor=\"0 0 255\"/>\n"
        "    <part name=\"Atlas\" AtlasImage=\"images/atlas.png\" AtlasRect=\"4 0 8 4\"\n"
        "          SizingType=\"TrueSize\"/>\n"
        "    <part name=\"Checker\" ImageFile=\"images/checker.png\" SizingType=\"TrueSize\"/>\n"
        "    <part name=\"Outside\" ImageFile=\"../theme.xml\"/>\n"
        "  </class>\n"
        "  <class name=\"Progress\">\n"
        "    <part ProgressChunkSize=\"4\" ProgressSpaceSize=\"2\"/>\n"
        "    <part name=\"Bar\" ImageFile=\"images/bar.png\" SizingMargins=\"2 2 2 2\"/>\n"
        "    <part name=\"Chunk\" ImageFile=\"images/chunk.png\"/>\n"
        "  </class>\n"
        "  <class name=\"Menu\">\n"
        "    <part name=\"PopupItem\" ImageFile=\"images/states.png\" ImageCount=\"3\"\n"
        "          ImageLayout=\"Vertical\"/>\n"
        "  </class>\n"
        "</style>\n");
    /* A 12 px 9-grid: a colour per corner, the edges one, the middle one. */
    uint32_t grid[12 * 12];
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 12; x++) {
            int cx = x < 4 ? 0 : x < 8 ? 1 : 2, cy = y < 4 ? 0 : y < 8 ? 1 : 2;
            static const uint32_t cells[3][3] = {{0xFF0000, 0x00FFFF, 0x00FF00},
                                                 {0x00FFFF, 0x808080, 0x00FFFF},
                                                 {0x0000FF, 0x00FFFF, 0xFFFF00}};
            grid[y * 12 + x] = cells[cy][cx];
        }
    pkg_png(root, "grid.png", 12, 12, grid);
    uint32_t states[4 * 12];
    for (int i = 0; i < 4 * 12; i++)
        states[i] = i < 16 ? 0x110000 : i < 32 ? 0x002200 : 0x000033;
    pkg_png(root, "states.png", 4, 12, states);
    uint32_t hstrip[8 * 4];
    for (int i = 0; i < 8 * 4; i++)
        hstrip[i] = i % 8 < 4 ? 0x0A0A0A : 0x0B0B0B;
    pkg_png(root, "hstrip.png", 8, 4, hstrip);
    pkg_png_solid(root, "small.png", 6, 4, 0xAA0000);
    pkg_png_solid(root, "dpi13.png", 13, 13, 0x101010);
    pkg_png_solid(root, "dpi26.png", 26, 26, 0x202020);
    pkg_png_solid(root, "size8.png", 8, 8, 0x303030);
    pkg_png_solid(root, "size16.png", 16, 16, 0x404040);
    pkg_png_solid(root, "glyph7.png", 7, 7, 0x505050);
    pkg_png_solid(root, "glyph11.png", 11, 11, 0x606060);
    uint32_t atlas[12 * 4];
    for (int i = 0; i < 12 * 4; i++)
        atlas[i] = i % 12 < 4 ? 0x700000 : i % 12 < 8 ? 0x007000 : 0x000070;
    pkg_png(root, "atlas.png", 12, 4, atlas);
    const uint32_t checker[4] = {0x000000, 0xFFFFFF, 0xFFFFFF, 0x000000};
    pkg_png(root, "checker.png", 2, 2, checker);
    pkg_png_solid(root, "bar.png", 10, 10, 0xC0C0C0);
    pkg_png_solid(root, "chunk.png", 1, 1, 0x00A000);
}

#if XD_HAVE_LIBARCHIVE
static void zip_test_package(const char *root, const char *zip) {
    struct archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    CHECK(archive_write_open_filename(writer, zip) == ARCHIVE_OK);
    for (size_t i = 0; i < G_N_ELEMENTS(PKG_FILES); i++) {
        char *path = g_build_filename(root, PKG_FILES[i], nullptr);
        char *data = nullptr;
        gsize len = 0;
        CHECK(g_file_get_contents(path, &data, &len, nullptr));
        struct archive_entry *member = archive_entry_new();
        archive_entry_set_pathname(member, PKG_FILES[i]);
        archive_entry_set_filetype(member, AE_IFREG);
        archive_entry_set_perm(member, 0644);
        archive_entry_set_size(member, (la_int64_t)len);
        CHECK(archive_write_header(writer, member) == ARCHIVE_OK);
        CHECK(archive_write_data(writer, data, len) == (la_ssize_t)len);
        archive_entry_free(member);
        g_free(data);
        g_free(path);
    }
    CHECK(archive_write_close(writer) == ARCHIVE_OK);
    archive_write_free(writer);
}
#endif

static bool prop_is(const xd_pkg *package, const char *cls, const char *part, const char *state,
                    const char *prop, const char *want) {
    const char *value = xd_pkg_prop(package, cls, part, state, prop);
    if (want == nullptr)
        return value == nullptr;
    return value != nullptr && strcmp(value, want) == 0;
}

/* A white canvas, w x h. */
static cairo_surface_t *pkg_canvas(int width, int height, cairo_t **cairo) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    *cairo = cairo_create(surface);
    cairo_set_source_rgb(*cairo, 1, 1, 1);
    cairo_paint(*cairo);
    return surface;
}

/* Draws part of class Draw in state at (0, 0, w, h) at `scale`, into a
 * canvas of cw x ch. */
static cairo_surface_t *pkg_render(const xd_pkg *package, double scale, const char *part,
                                   const char *state, int width, int height, int cw, int ch) {
    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_scheme sc;
    xd_scheme_init_default(&sc);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, scale);
    cairo_t *cairo;
    cairo_surface_t *surface = pkg_canvas(cw, ch, &cairo);
    CHECK(xd_pkg_draw(&theme, cairo, "Draw", part, state, (xd_rect){0, 0, width, height}));
    cairo_destroy(cairo);
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    return surface;
}

static int pkg_part_w(const xd_pkg *package, double scale, const char *part) {
    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_scheme sc;
    xd_scheme_init_default(&sc);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, scale);
    int width = -1, height = -1;
    CHECK(xd_pkg_part_size(&theme, "Draw", part, "1", &width, &height));
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    return width == height ? width : -1;
}

/* The taskbar, Start menus and their menus from a package (style_pkg.c's
 * shell section): flat colours of our own, checked where nothing else is
 * drawn (no logo or text). */
static void make_shell_package(const char *root) {
    char *styles = g_build_filename(root, "styles", nullptr);
    char *images = g_build_filename(root, "images", nullptr);
    CHECK(g_mkdir(styles, 0755) == 0 && g_mkdir(images, 0755) == 0);
    g_free(styles);
    g_free(images);
    pkg_text(root, "theme.xml",
             "<xdtheme version=\"1\" name=\"Shell Test\" base=\"luna\">\n"
             "  <variant id=\"a\" name=\"A\" size=\"normal\" style=\"styles/a.xml\"/>\n"
             "</xdtheme>\n");
    pkg_text(
        root, "styles/a.xml",
        "<style>\n"
        "  <class name=\"TaskBar\">\n"
        "    <part name=\"BackgroundBottom\" ImageFile=\"images/bar.png\"/>\n"
        "    <part name=\"SizingBarBottom\" ImageFile=\"images/sizing.png\" SizingType=\"Tile\"/>\n"
        "  </class>\n"
        "  <class name=\"Start::Button\">\n"
        "    <part ImageFile=\"images/start.png\" ImageCount=\"3\" ImageLayout=\"Vertical\"\n"
        "          Font=\"Tahoma, 8, Bold\" TextColor=\"1 2 3\"/>\n"
        "  </class>\n"
        "  <class name=\"Button\"><part name=\"PushButton\" ImageFile=\"images/bar.png\"/>\n"
        "    <part ImageFile=\"images/bar.png\"/></class>\n"
        "  <class name=\"TaskBand::Toolbar\">\n"
        "    <part TextColor=\"255 255 255\"/>\n"
        "    <part name=\"Button\" ImageFile=\"images/task.png\" ImageCount=\"6\"\n"
        "          ImageLayout=\"Vertical\"/>\n"
        "  </class>\n"
        "  <class name=\"TaskBand\"><part name=\"FlashButton\" ImageFile=\"images/flash.png\"/>\n"
        "  </class>\n"
        "  <class name=\"TrayNotifyHoriz::TrayNotify\">\n"
        "    <part name=\"Background\" ImageFile=\"images/tray.png\"/>\n"
        "  </class>\n"
        "  <class name=\"TrayNotify::Clock\"><part TextColor=\"10 20 30\"/></class>\n"
        "  <class name=\"TrayNotifyHoriz::Button\">\n"
        "    <part ImageFile=\"images/states.png\" ImageCount=\"3\" ImageLayout=\"Vertical\"\n"
        "          SizingType=\"TrueSize\"/>\n"
        "  </class>\n"
        "  <class name=\"StartPanel\">\n"
        "    <part name=\"ProgList\" ImageFile=\"images/prog.png\" TextColor=\"11 22 33\"\n"
        "          CaptionText=\"44 55 66\"/>\n"
        "    <part name=\"LogoffButtons\" ImageFile=\"images/logoff.png\" SizingType=\"TrueSize\"/>\n"
        "  </class>\n"
        "  <class name=\"StartMenu::Toolbar\">\n"
        "    <part ImageFile=\"images/menu.png\" TextColor=\"0 0 0\"/>\n"
        "    <part state=\"Hot\" TextColor=\"9 9 9\"/>\n"
        "    <part name=\"Button\" ImageFile=\"images/states.png\" ImageCount=\"3\"\n"
        "          ImageLayout=\"Vertical\"/>\n"
        "  </class>\n"
        "  <class name=\"StartMenu::MenuBand\">\n"
        "    <part name=\"NewAppButton\" ImageFile=\"images/new.png\"/>\n"
        "  </class>\n"
        "  <class name=\"ClassicStartMenu\">\n"
        "    <part name=\"Banner\" ImageFile=\"images/banner.png\" TextColor=\"1 1 1\"/>\n"
        "    <part name=\"Item\" state=\"Hot\" ImageFile=\"images/item.png\"/>\n"
        "  </class>\n"
        "  <class name=\"TaskBand2\">\n"
        "    <part name=\"AppButton\" ImageFile=\"images/app.png\" ImageCount=\"7\"\n"
        "          ImageLayout=\"Vertical\"/>\n"
        "    <part name=\"ProgressError\" ImageFile=\"images/red.png\"/>\n"
        "  </class>\n"
        "  <class name=\"JumpList\">\n"
        "    <part name=\"Item\" state=\"Hot\" ImageFile=\"images/item.png\" TextColor=\"7 7 7\"/>\n"
        "  </class>\n"
        "</style>\n");
    pkg_png_solid(root, "bar.png", 4, 4, 0x2040C0);
    pkg_png_solid(root, "sizing.png", 4, 3, 0x00FF00);
    uint32_t start[4 * 12], task[2 * 12], app[2 * 14], states[4 * 12];
    for (int i = 0; i < 4 * 12; i++) {
        start[i] = 0x100000u * (uint32_t)(i / 16 + 1);
        states[i] = i < 16 ? 0x110000 : i < 32 ? 0x002200 : 0x000033;
    }
    for (int i = 0; i < 2 * 12; i++)
        task[i] = 0x000010u * (uint32_t)(i / 4 + 1);
    for (int i = 0; i < 2 * 14; i++)
        app[i] = 0x001000u * (uint32_t)(i / 4 + 1);
    pkg_png(root, "start.png", 4, 12, start);
    pkg_png(root, "task.png", 2, 12, task);
    pkg_png(root, "app.png", 2, 14, app);
    pkg_png(root, "states.png", 4, 12, states);
    pkg_png_solid(root, "flash.png", 2, 2, 0xFF8000);
    pkg_png_solid(root, "tray.png", 2, 2, 0x0080FF);
    pkg_png_solid(root, "prog.png", 2, 2, 0xEEEEEE);
    uint32_t logoff[12 * 4];
    for (int i = 0; i < 12 * 4; i++)
        logoff[i] = 0x111111u * (uint32_t)(i % 12 / 4 + 1);
    pkg_png(root, "logoff.png", 12, 4, logoff);
    pkg_png_solid(root, "menu.png", 2, 2, 0xABCDEF);
    pkg_png_solid(root, "new.png", 2, 2, 0xFEDCBA);
    pkg_png_solid(root, "banner.png", 2, 2, 0x123456);
    pkg_png_solid(root, "item.png", 2, 2, 0x654321);
    pkg_png_solid(root, "red.png", 2, 2, 0xE00000);
}

static void test_theme_package_shell(void) {
    const char *names = XD_SOURCE_ROOT "/data/theme/uxtheme.ini";
    char *root = g_dir_make_tmp("xd-pkg-XXXXXX", nullptr);
    make_shell_package(root);
    GError *error = nullptr;
    xd_pkg *package = xd_pkg_load(root, nullptr, names, &error);
    CHECK(package != nullptr && error == nullptr);
    if (package == nullptr) {
        g_clear_error(&error);
        rm_tree(root);
        g_free(root);
        return;
    }

    /* Font properties, and a class's own parts apart from its base's. */
    xd_font_spec font = {0};
    CHECK(xd_pkg_font(package, "Start::Button", nullptr, nullptr, "Font", &font, nullptr) &&
          font.size_pt == 8 && font.bold && !font.italic &&
          strstr(font.family, "Tahoma") != nullptr);
    CHECK(!xd_pkg_font(package, "StartPanel", "ProgList", nullptr, "TextColor", &font, nullptr));
    CHECK(xd_pkg_has_own(package, "Start::Button", nullptr) &&
          !xd_pkg_has_own(package, "Dialog::Button", nullptr));
    CHECK(xd_pkg_has(package, "Dialog::Button", "PushButton"));
    /* Deduced and our own names resolve as uxtheme.ini lists them. */
    CHECK(prop_is(package, "ClassicStartMenu", "3", "2", "ImageFile", "images/item.png"));

    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_scheme sc;
    xd_scheme_init_default(&sc);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, 1.0);
    CHECK(theme.style == XD_STYLE_LUNA && theme.menu_look == XD_MENU_LOOK_NORMAL);
    cairo_t *cairo;
    cairo_surface_t *surface = pkg_canvas(60, 30, &cairo);

    /* The taskbar, and an unlocked one's sizing bar along its top. */
    xd_theme_draw_taskbar(&theme, cairo, (xd_rect){0, 0, 60, 30}, XD_EDGE_BOTTOM);
    CHECK(pixel(surface, 5, 1) == 0x2040C0 && pixel(surface, 59, 29) == 0x2040C0);
    xd_theme_draw_taskbar_sizing_bar(&theme, cairo, (xd_rect){0, 0, 60, 30}, XD_EDGE_BOTTOM);
    CHECK(pixel(surface, 5, 0) == 0x00FF00 && pixel(surface, 30, 2) == 0x00FF00 &&
          pixel(surface, 5, 3) == 0x2040C0);

    /* The Start button's states: Normal, Hot, Pressed. */
    static const struct {
        uint8_t state;
        uint32_t want;
    } starts[] = {{0, 0x100000}, {XD_STATE_HOT, 0x200000}, {XD_STATE_PRESSED, 0x300000}};
    for (size_t i = 0; i < G_N_ELEMENTS(starts); i++) {
        xd_theme_draw_start_button(&theme, cairo, (xd_rect){0, 0, 60, 30}, starts[i].state);
        CHECK(pixel(surface, 1, 1) == starts[i].want);
    }

    /* Task buttons: toolbar states, Checked for the active window, the
     * flashing one TaskBand's; a side taskbar's falls back to TaskBand's. */
    static const struct {
        uint8_t state;
        uint32_t want;
    } tasks[] = {
        {0, 0x000010},
        {XD_STATE_HOT, 0x000020},
        {XD_STATE_CHECKED, 0x000050},
        {XD_STATE_CHECKED | XD_STATE_HOT, 0x000060},
        {XD_STATE_FLASH, 0xFF8000},
        {XD_STATE_CHECKED | XD_STATE_FLASH, 0x000050},
        {XD_STATE_VERTICAL, 0x000010},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(tasks); i++) {
        xd_theme_draw_task_button(&theme, cairo, (xd_rect){0, 0, 60, 30}, tasks[i].state, "x",
                                  nullptr);
        CHECK(pixel(surface, 1, 1) == tasks[i].want && pixel(surface, 58, 28) == tasks[i].want);
    }
    xd_theme_draw_task_group(&theme, cairo, (xd_rect){0, 0, 60, 30}, XD_STATE_CHECKED, 3, "x",
                             nullptr);
    CHECK(pixel(surface, 1, 1) == 0x000050);

    /* The tray, its clock's colour, its chevron (none for a side taskbar:
     * Button's push button doesn't stand in for it). */
    xd_theme_draw_tray(&theme, cairo, (xd_rect){0, 0, 60, 30}, "");
    CHECK(pixel(surface, 1, 1) == 0x0080FF);
    xd_rgba ck = xd_theme_clock_color(&theme);
    CHECK(lround(ck.r * 255) == 10 && lround(ck.g * 255) == 20 && lround(ck.b * 255) == 30);
    CHECK(xd_theme_draw_tray_chevron(&theme, cairo, (xd_rect){0, 0, 4, 4}, XD_STATE_HOT, false,
                                     false) &&
          pixel(surface, 1, 1) == 0x002200);
    CHECK(!xd_theme_draw_tray_chevron(&theme, cairo, (xd_rect){0, 0, 4, 4}, 0, false, true));
    CHECK(!xd_theme_draw_tray_chevron(&theme, cairo, (xd_rect){0, 0, 4, 4}, 0, true, false));
    CHECK(!xd_theme_draw_show_desktop(&theme, cairo, (xd_rect){0, 0, 4, 4}, 0, false));

    /* XP's Start panel: its parts, colours, and Log Off's and Turn Off's
     * icons, the last two squares of LogoffButtons. */
    CHECK(xd_theme_draw_start_part(&theme, cairo, XD_START_PROG_LIST, (xd_rect){0, 0, 60, 30}, 0) &&
          pixel(surface, 30, 15) == 0xEEEEEE);
    CHECK(!xd_theme_draw_start_part(&theme, cairo, XD_START_USER_PANE, (xd_rect){0, 0, 60, 30}, 0));
    xd_rgba color;
    CHECK(xd_theme_start_part_color(&theme, XD_START_PROG_LIST, 0, XD_START_TEXT, &color) &&
          lround(color.b * 255) == 33);
    CHECK(xd_theme_start_part_color(&theme, XD_START_PROG_LIST, 0, XD_START_SUBTEXT, &color) &&
          lround(color.r * 255) == 44);
    CHECK(xd_theme_draw_start_logoff_icon(&theme, cairo, (xd_rect){0, 0, 8, 8}, false, 0) &&
          pixel(surface, 3, 3) == 0x222222 && pixel(surface, 4, 4) == 0x222222);
    CHECK(xd_theme_draw_start_logoff_icon(&theme, cairo, (xd_rect){0, 0, 8, 8}, true, 0) &&
          pixel(surface, 3, 3) == 0x333333 && pixel(surface, 1, 1) == 0xEEEEEE);

    /* Menus: ordinary ones as before; the Start menus' cascades in
     * StartMenu::Toolbar, the Windows 2000 one in ClassicStartMenu. */
    xd_theme_draw_menu_frame(&theme, cairo, (xd_rect){0, 0, 60, 30});
    CHECK(pixel(surface, 30, 15) != 0xABCDEF);
    theme.menu_look = XD_MENU_LOOK_START;
    xd_theme_draw_menu_frame(&theme, cairo, (xd_rect){0, 0, 60, 30});
    CHECK(pixel(surface, 30, 15) == 0xABCDEF);
    xd_theme_draw_menu_item(&theme, cairo, (xd_rect){0, 0, 60, 22}, XD_STATE_HOT, "", false, false);
    CHECK(pixel(surface, 1, 1) == 0x002200);
    CHECK(xd_theme_draw_new_program(&theme, cairo, (xd_rect){0, 0, 60, 30}) &&
          pixel(surface, 30, 15) == 0xFEDCBA);
    theme.menu_look = XD_MENU_LOOK_CLASSIC_START;
    xd_theme_draw_menu_item(&theme, cairo, (xd_rect){0, 0, 60, 22}, XD_STATE_HOT, "", false, false);
    CHECK(pixel(surface, 1, 1) == 0x654321);
    xd_theme_draw_start_banner(&theme, cairo, (xd_rect){0, 0, 20, 30}, "", "");
    CHECK(pixel(surface, 1, 1) == 0x123456);
    theme.menu_look = XD_MENU_LOOK_NORMAL;

    /* 7's program button (AppButton: active 4), the error progress over
     * its left half. */
    xd_app_button ab = {.windows = 1, .glow_x = -1, .progress = XD_PROGRESS_ERROR,
                        .fraction = 0.5};
    xd_theme_draw_app_button(&theme, cairo, (xd_rect){0, 0, 60, 30}, XD_STATE_CHECKED, &ab);
    CHECK(pixel(surface, 3, 15) == 0xE00000 && pixel(surface, 50, 15) == 0x004000);
    /* Jump list items: JumpList's hot item and its text colour. */
    xd_theme_draw_jumplist_item(&theme, cairo, (xd_rect){0, 0, 60, 22}, XD_STATE_HOT, "", 30);
    CHECK(pixel(surface, 1, 1) == 0x654321);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);

    /* Without a package: 7's own progress colours. */
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_theme_init(&theme, XD_STYLE_AERO, &sc, 1.0);
    surface = pkg_canvas(20, 10, &cairo);
    xd_theme_draw_taskbar_progress(&theme, cairo, (xd_rect){0, 0, 20, 10}, XD_PROGRESS_NORMAL, 0.5);
    uint32_t bar_pixel = pixel(surface, 2, 5);
    CHECK(((bar_pixel >> 8) & 0xFF) > ((bar_pixel >> 16) & 0xFF) + 40 &&
          pixel(surface, 15, 5) == 0xFFFFFF);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);

    xd_pkg_free(package);
    rm_tree(root);
    g_free(root);
}

static void test_theme_package(void) {
    const char *names = XD_SOURCE_ROOT "/data/theme/uxtheme.ini";
    char *root = g_dir_make_tmp("xd-pkg-XXXXXX", nullptr);
    char *dir = g_build_filename(root, "test", nullptr);
    CHECK(g_mkdir(dir, 0755) == 0);
    make_test_package(dir);

    /* The loader: a folder, its variants (the first unless one is
     * named), and errors. */
    GError *error = nullptr;
    xd_pkg *package = xd_pkg_load(dir, nullptr, names, &error);
    CHECK(package != nullptr && error == nullptr);
    if (package == nullptr) {
        g_clear_error(&error);
        rm_tree(root);
        g_free(dir);
        g_free(root);
        return;
    }
    CHECK(strcmp(xd_pkg_name(package), "Test Style") == 0 &&
          strcmp(xd_pkg_variant(package), "a") == 0);
    CHECK(xd_pkg_base(package) == XD_STYLE_WHISTLER);
    xd_image_filter image_filter = XD_FILTER_SMOOTH;
    CHECK(xd_pkg_filter(package, &image_filter) && image_filter == XD_FILTER_INTEGER);
    xd_pkg *pb = xd_pkg_load(dir, "b", names, nullptr);
    CHECK(pb != nullptr && strcmp(xd_pkg_variant(pb), "b") == 0);
    CHECK(pb != nullptr && prop_is(pb, "Button", "PushButton", nullptr, "Lvl", "b"));
    xd_pkg_free(pb);
    pb = xd_pkg_load(dir, "no-such-variant", names, nullptr);
    CHECK(pb != nullptr && strcmp(xd_pkg_variant(pb), "a") == 0);
    xd_pkg_free(pb);
    char *missing = g_build_filename(root, "none", nullptr);
    CHECK(xd_pkg_load(missing, nullptr, names, &error) == nullptr && error != nullptr);
    g_clear_error(&error);
    g_free(missing);
#if XD_HAVE_LIBARCHIVE
    char *zip = g_build_filename(root, "test.xdtheme", nullptr);
    zip_test_package(dir, zip);
    xd_pkg *pz = xd_pkg_load(zip, "b", names, &error);
    CHECK(pz != nullptr && error == nullptr);
    if (pz != nullptr) {
        CHECK(strcmp(xd_pkg_name(pz), "Test Style") == 0 && strcmp(xd_pkg_variant(pz), "b") == 0);
        xd_pkg_free(pz);
    }
    g_clear_error(&error);
    pz = xd_pkg_load(zip, nullptr, names, nullptr);
    CHECK(pz != nullptr);
    if (pz != nullptr) {
        /* Images come out of the zip too. */
        cairo_surface_t *surface = pkg_render(pz, 1, "Small", "1", 20, 20, 20, 20);
        CHECK(pixel(surface, 7, 8) == 0xAA0000);
        cairo_surface_destroy(surface);
        xd_pkg_free(pz);
    }
    char *notzip = g_build_filename(dir, "theme.xml", nullptr);
    CHECK(xd_pkg_load(notzip, nullptr, names, &error) == nullptr && error != nullptr);
    g_clear_error(&error);
    g_free(notzip);
    g_free(zip);
#endif

    /* Lookups: part and state, part, class, Class for App::Class,
     * ClassStyle, globals; names and numbers alike, any case. */
    CHECK(prop_is(package, "Button", "PushButton", "Hot", "Lvl", "state"));
    CHECK(prop_is(package, "button", "PUSHBUTTON", "hot", "lvl", "state"));
    CHECK(prop_is(package, "Button", "1", "2", "Lvl", "state"));
    CHECK(prop_is(package, "Button", "PushButton", "Pressed", "Lvl", "part"));
    CHECK(prop_is(package, "Button", "PushButton", nullptr, "Lvl", "part"));
    CHECK(prop_is(package, "Button", "CheckBox", "Hot", "Lvl", "checkbox"));
    CHECK(prop_is(package, "Button", "RadioButton", nullptr, "Lvl", "class"));
    CHECK(prop_is(package, "Dialog::Button", "PushButton", "Hot", "Lvl", "app"));
    CHECK(prop_is(package, "Dialog::Button", "PushButton", "Normal", "Lvl", "part"));
    CHECK(prop_is(package, "Dialog::Button", "PushButton", nullptr, "PartOnly", "p"));
    CHECK(prop_is(package, "Dialog::Button", "GroupBox", nullptr, "Lvl", "class"));
    CHECK(prop_is(package, "Button", "PushButton", nullptr, "StyleProp", "s"));
    CHECK(prop_is(package, "Edit", "EditText", "Normal", "Glob", "g"));
    CHECK(prop_is(package, "Button", nullptr, nullptr, "NoSuchProperty", nullptr));
    /* A state on the class itself, Toolbar(Hot): Hot is 2 for all its parts. */
    CHECK(prop_is(package, "Toolbar", nullptr, "2", "TextColor", "1 2 3"));
    CHECK(prop_is(package, "Toolbar", "0", "HOT", "TextColor", "1 2 3"));
    CHECK(prop_is(package, "Toolbar", nullptr, "Pressed", "TextColor", nullptr));
    xd_rgba rgba;
    CHECK(xd_pkg_color(package, "Toolbar", nullptr, "Hot", "TextColor", &rgba) &&
          fabs(rgba.r - 1 / 255.0) < 1e-9 && fabs(rgba.b - 3 / 255.0) < 1e-9);
    /* Which parts it styles, through the same classes. */
    CHECK(xd_pkg_has(package, "Button", "PushButton") &&
          xd_pkg_has(package, "Dialog::Button", "CheckBox"));
    CHECK(xd_pkg_has(package, "Button", "UserButton")); /* in ButtonStyle */
    CHECK(!xd_pkg_has(package, "Button", "GroupBox") && !xd_pkg_has(package, "Tab", "Pane"));

    /* [SysMetrics] over the scheme, with the base style. */
    xd_scheme sc;
    xd_scheme_init_default(&sc);
    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, 1.0);
    CHECK(theme.pkg == package && theme.style == XD_STYLE_WHISTLER);
    CHECK(rgb_is(theme.scheme.colors[XD_COLOR_BUTTON_FACE], 1, 2, 3));
    CHECK(rgb_is(theme.scheme.colors[XD_COLOR_WINDOW], 0x10, 0x20, 0x30));
    CHECK(rgb_is(theme.scheme.colors[XD_COLOR_HILIGHT], sc.colors[XD_COLOR_HILIGHT].r,
                 sc.colors[XD_COLOR_HILIGHT].g, sc.colors[XD_COLOR_HILIGHT].b));
    CHECK(theme.scheme.metrics[XD_METRIC_CAPTION_HEIGHT] == 27);
    CHECK(theme.scheme.metrics[XD_METRIC_SCROLL_WIDTH] == 19);
    CHECK(theme.scheme.metrics[XD_METRIC_MENU_HEIGHT] == sc.metrics[XD_METRIC_MENU_HEIGHT]);
    const xd_font_spec *cap = &theme.scheme.fonts[XD_FONT_CAPTION];
    CHECK(strcmp(cap->family, XD_FONT_FAMILY_TAHOMA) == 0 && cap->size_pt == 11 && cap->bold);
    const xd_font_spec *menu = &theme.scheme.fonts[XD_FONT_MENU];
    CHECK(g_str_has_prefix(menu->family, "Comic Neue") && menu->size_pt == 9 && !menu->bold);
    CHECK(strcmp(theme.scheme.fonts[XD_FONT_ICON].family, sc.fonts[XD_FONT_ICON].family) == 0);
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, 1.0);
    CHECK(theme.pkg == nullptr && theme.style == XD_STYLE_CLASSIC);

    /* 9-grid: corners as they are, edges and middle stretched. */
    cairo_surface_t *surface = pkg_render(package, 1, "Grid", "1", 40, 30, 42, 32);
    CHECK(pixel(surface, 0, 0) == 0xFF0000 && pixel(surface, 3, 3) == 0xFF0000);
    CHECK(pixel(surface, 39, 0) == 0x00FF00 && pixel(surface, 36, 3) == 0x00FF00);
    CHECK(pixel(surface, 0, 29) == 0x0000FF && pixel(surface, 39, 29) == 0xFFFF00);
    CHECK(pixel(surface, 36, 26) == 0xFFFF00 && pixel(surface, 35, 26) == 0x00FFFF);
    CHECK(pixel(surface, 4, 0) == 0x00FFFF && pixel(surface, 20, 3) == 0x00FFFF &&
          pixel(surface, 0, 15) == 0x00FFFF);
    CHECK(pixel(surface, 4, 4) == 0x808080 && pixel(surface, 20, 15) == 0x808080 &&
          pixel(surface, 35, 25) == 0x808080);
    CHECK(pixel(surface, 40, 0) == 0xFFFFFF && pixel(surface, 0, 30) == 0xFFFFFF);
    cairo_surface_destroy(surface);

    /* TrueSize: as it is, centred. */
    surface = pkg_render(package, 1, "Small", "1", 20, 20, 20, 20);
    CHECK(pixel(surface, 7, 8) == 0xAA0000 && pixel(surface, 12, 11) == 0xAA0000);
    CHECK(pixel(surface, 6, 8) == 0xFFFFFF && pixel(surface, 13, 8) == 0xFFFFFF);
    CHECK(pixel(surface, 7, 7) == 0xFFFFFF && pixel(surface, 7, 12) == 0xFFFFFF);
    cairo_surface_destroy(surface);

    /* A state's frame: number - 1; past the last, the last. */
    static const struct {
        const char *state;
        uint32_t want;
    } frames[] = {{"1", 0x110000}, {"2", 0x002200}, {"3", 0x000033}, {"9", 0x000033}};
    for (size_t i = 0; i < G_N_ELEMENTS(frames); i++) {
        surface = pkg_render(package, 1, "States", frames[i].state, 4, 4, 4, 4);
        CHECK(pixel(surface, 0, 0) == frames[i].want && pixel(surface, 3, 3) == frames[i].want);
        cairo_surface_destroy(surface);
    }
    /* Without an ImageLayout, frames are side by side, as uxtheme has them. */
    surface = pkg_render(package, 1, "Strip", "2", 4, 4, 4, 4);
    CHECK(pixel(surface, 0, 0) == 0x0B0B0B && pixel(surface, 3, 3) == 0x0B0B0B);
    cairo_surface_destroy(surface);
    surface = pkg_render(package, 2, "Strip", "1", 8, 8, 8, 8);
    CHECK(pixel(surface, 0, 0) == 0x0A0A0A && pixel(surface, 7, 7) == 0x0A0A0A);
    cairo_surface_destroy(surface);
    /* By name: PushButton's Hot is 2, Pressed 3. */
    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, 1.0);
    cairo_t *cairo;
    surface = pkg_canvas(4, 4, &cairo);
    CHECK(xd_pkg_draw(&theme, cairo, "Button", "PushButton", "Hot", (xd_rect){0, 0, 4, 4}));
    CHECK(pixel(surface, 1, 1) == 0x002200);
    CHECK(xd_pkg_draw(&theme, cairo, "Button", "PushButton", "pressed", (xd_rect){0, 0, 4, 4}));
    CHECK(pixel(surface, 1, 1) == 0x000033);
    /* Nothing outside the package, nor for parts it lacks. */
    CHECK(!xd_pkg_draw(&theme, cairo, "Draw", "Outside", "1", (xd_rect){0, 0, 4, 4}));
    CHECK(!xd_pkg_draw(&theme, cairo, "Draw", "NoSuchPart", "1", (xd_rect){0, 0, 4, 4}));
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);

    /* ImageFileN by DPI: the one made for it unscaled, else the nearest
     * below, scaled. */
    CHECK(pkg_part_w(package, 1, "Dpi") == 13);
    CHECK(pkg_part_w(package, 2, "Dpi") == 26);
    CHECK(pkg_part_w(package, 1.5, "Dpi") == 20);
    CHECK(pkg_part_w(package, 3, "Dpi") == 39);
    surface = pkg_render(package, 2, "Dpi", "1", 26, 26, 26, 26);
    CHECK(pixel(surface, 0, 0) == 0x202020 && pixel(surface, 25, 25) == 0x202020);
    cairo_surface_destroy(surface);
    /* ... and by size: the largest whose MinSize fits. */
    surface = pkg_render(package, 1, "BySize", "1", 10, 10, 20, 20);
    CHECK(pixel(surface, 5, 5) == 0x303030);
    cairo_surface_destroy(surface);
    surface = pkg_render(package, 1, "BySize", "1", 20, 20, 20, 20);
    CHECK(pixel(surface, 10, 10) == 0x404040 && pixel(surface, 2, 2) == 0x404040 &&
          pixel(surface, 1, 1) == 0xFFFFFF);
    cairo_surface_destroy(surface);
    /* Image glyphs: the largest that fits, centred. */
    surface = pkg_render(package, 1, "Glyph", "1", 10, 10, 14, 14);
    CHECK(pixel(surface, 1, 1) == 0x505050 && pixel(surface, 7, 7) == 0x505050);
    CHECK(pixel(surface, 0, 0) == 0xFFFFFF && pixel(surface, 8, 8) == 0xFFFFFF);
    cairo_surface_destroy(surface);
    surface = pkg_render(package, 1, "Glyph", "1", 14, 14, 14, 14);
    CHECK(pixel(surface, 1, 1) == 0x606060 && pixel(surface, 11, 11) == 0x606060 &&
          pixel(surface, 12, 12) == 0xFFFFFF);
    cairo_surface_destroy(surface);

    /* BorderFill, its border scaled. */
    surface = pkg_render(package, 1, "Border", "1", 10, 10, 10, 10);
    CHECK(pixel(surface, 0, 0) == 0xFF0000 && pixel(surface, 1, 1) == 0xFF0000 &&
          pixel(surface, 9, 9) == 0xFF0000);
    CHECK(pixel(surface, 2, 2) == 0x0000FF && pixel(surface, 7, 7) == 0x0000FF &&
          pixel(surface, 8, 5) == 0xFF0000);
    cairo_surface_destroy(surface);
    surface = pkg_render(package, 2, "Border", "1", 20, 20, 20, 20);
    CHECK(pixel(surface, 3, 3) == 0xFF0000 && pixel(surface, 4, 4) == 0x0000FF &&
          pixel(surface, 16, 10) == 0xFF0000);
    cairo_surface_destroy(surface);

    /* AtlasRect: a rectangle of a larger image. */
    surface = pkg_render(package, 1, "Atlas", "1", 4, 4, 4, 4);
    CHECK(pixel(surface, 0, 0) == 0x007000 && pixel(surface, 3, 3) == 0x007000);
    cairo_surface_destroy(surface);

    /* High DPI: at whole-number scales, each pixel an exact block. */
    for (int k = 2; k <= 3; k++) {
        surface = pkg_render(package, k, "Checker", "1", 2 * k, 2 * k, 2 * k, 2 * k);
        bool exact = true;
        for (int y = 0; y < 2 * k; y++)
            for (int x = 0; x < 2 * k; x++)
                exact = exact && pixel(surface, x, y) == ((x / k + y / k) % 2 ? 0xFFFFFFu : 0u);
        CHECK(exact);
        cairo_surface_destroy(surface);
    }

    /* The controls drawn from it: progress chunks ProgressChunkSize wide,
     * ProgressSpaceSize apart, inside the bar's SizingMargins; a menu's hot
     * item from Menu's PopupItem, nothing behind a normal one. */
    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, 1.0);
    surface = pkg_canvas(40, 10, &cairo);
    xd_theme_draw_progress(&theme, cairo, (xd_rect){0, 0, 40, 10}, 0.5);
    CHECK(pixel(surface, 1, 5) == 0xC0C0C0 && pixel(surface, 2, 1) == 0xC0C0C0);
    CHECK(pixel(surface, 2, 2) == 0x00A000 && pixel(surface, 5, 7) == 0x00A000);
    CHECK(pixel(surface, 6, 5) == 0xC0C0C0 && pixel(surface, 7, 5) == 0xC0C0C0);
    CHECK(pixel(surface, 8, 5) == 0x00A000 && pixel(surface, 17, 5) == 0x00A000);
    CHECK(pixel(surface, 18, 5) == 0xC0C0C0 && pixel(surface, 20, 5) == 0xC0C0C0);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    surface = pkg_canvas(60, 44, &cairo);
    xd_theme_draw_menu_item(&theme, cairo, (xd_rect){0, 0, 60, 22}, XD_STATE_HOT, "", false, false);
    xd_theme_draw_menu_item(&theme, cairo, (xd_rect){0, 22, 60, 22}, 0, "", false, false);
    CHECK(pixel(surface, 30, 11) == 0x002200 && pixel(surface, 0, 0) == 0x002200);
    CHECK(pixel(surface, 30, 33) == 0xFFFFFF);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);

    xd_pkg_free(package);
    rm_tree(root);
    g_free(dir);
    g_free(root);
}

static void test_theme_settings(void) {
    /* [Appearance] IconTheme and GtkTheme: directory names, "" = default. */
    xd_settings original;
    xd_settings_parse(XD_STR("[Appearance]\nIconTheme=oxygen\nGtkTheme=Adwaita:dark\n"), &original);
    CHECK(strcmp(original.icon_theme, "oxygen") == 0 &&
          strcmp(original.gtk_theme, "Adwaita:dark") == 0);
    CHECK(strcmp(xd_settings_icon_theme(&original), "oxygen") == 0);
    CHECK(strcmp(xd_settings_gtk_theme(&original), "Adwaita:dark") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && strcmp(parsed.icon_theme, "oxygen") == 0);
    xd_strbuf_free(&text);
    /* No paths, nothing hidden, nothing too long. */
    xd_settings_parse(XD_STR("[Appearance]\nIconTheme=../../etc\nGtkTheme=.hidden\n"), &original);
    CHECK(original.icon_theme[0] == '\0' && original.gtk_theme[0] == '\0');
    xd_settings_parse(
        XD_STR("[Appearance]\nIconTheme=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"),
        &original);
    CHECK(original.icon_theme[0] == '\0');
    /* Unset: ours, and $XD_ICON_THEME before breeze. */
    xd_settings_defaults(&original);
    CHECK(strcmp(xd_settings_gtk_theme(&original), "XeonDesktop") == 0);
    char *env = g_strdup(g_getenv("XD_ICON_THEME"));
    g_unsetenv("XD_ICON_THEME");
    CHECK(strcmp(xd_settings_icon_theme(&original), "breeze") == 0);
    g_setenv("XD_ICON_THEME", "Adwaita", TRUE);
    CHECK(strcmp(xd_settings_icon_theme(&original), "Adwaita") == 0);
    snprintf(original.icon_theme, sizeof(original.icon_theme), "%s", "oxygen");
    CHECK(strcmp(xd_settings_icon_theme(&original), "oxygen") == 0); /* the setting wins */
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
    char *value = g_key_file_get_string(kf, "Settings", "gtk-theme-name", nullptr);
    CHECK(g_strcmp0(value, "Zap") == 0);
    g_free(value);
    value = g_key_file_get_string(kf, "Settings", "gtk-icon-theme-name", nullptr);
    CHECK(g_strcmp0(value, "zed") == 0);
    g_free(value);
    value = g_key_file_get_string(kf, "Settings", "gtk-font-name", nullptr);
    CHECK(g_strcmp0(value, "Sans 10") == 0);
    g_free(value);
    value = g_key_file_get_comment(kf, nullptr, nullptr, nullptr);
    CHECK(value != nullptr && strstr(value, "mine") != nullptr);
    g_free(value);
    g_key_file_free(kf);
    /* Ours: GTK4 has no such theme, so its file loses the key. */
    xd_gtk_settings_export(config, backend, "XeonDesktop", nullptr);
    kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini4, G_KEY_FILE_NONE, nullptr));
    CHECK(!g_key_file_has_key(kf, "Settings", "gtk-theme-name", nullptr));
    value = g_key_file_get_string(kf, "Settings", "gtk-icon-theme-name", nullptr);
    CHECK(g_strcmp0(value, "zed") == 0);
    g_free(value);
    g_key_file_free(kf);
    kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini3, G_KEY_FILE_NONE, nullptr));
    value = g_key_file_get_string(kf, "Settings", "gtk-theme-name", nullptr);
    CHECK(g_strcmp0(value, "XeonDesktop") == 0);
    g_free(value);
    g_key_file_free(kf);
    /* GSettings, where the schema is installed (in memory here). */
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema =
        src != nullptr ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE)
                       : nullptr;
    if (schema != nullptr) {
        GSettings *gsettings = g_settings_new_full(schema, backend, nullptr);
        value = g_settings_get_string(gsettings, "gtk-theme");
        CHECK(strcmp(value, "XeonDesktop") == 0);
        g_free(value);
        value = g_settings_get_string(gsettings, "icon-theme");
        CHECK(strcmp(value, "zed") == 0);
        g_free(value);
        /* After a rewrite, ours switches between its two names (GTK only
         * reloads a theme when the name changes)... */
        CHECK(xd_gtk_theme_nudge(backend));
        value = g_settings_get_string(gsettings, "gtk-theme");
        CHECK(strcmp(value, XD_GTK_THEME_ALT_NAME) == 0);
        g_free(value);
        CHECK(xd_gtk_theme_nudge(backend));
        value = g_settings_get_string(gsettings, "gtk-theme");
        CHECK(strcmp(value, XD_GTK_THEME_NAME) == 0);
        g_free(value);
        /* ...but another theme set there stays. */
        g_settings_set_string(gsettings, "gtk-theme", "Zap");
        CHECK(!xd_gtk_theme_nudge(backend));
        value = g_settings_get_string(gsettings, "gtk-theme");
        CHECK(strcmp(value, "Zap") == 0);
        g_free(value);
        g_object_unref(gsettings);
        /* Nothing set at all: ours goes in. */
        GSettingsBackend *fresh = g_memory_settings_backend_new();
        CHECK(xd_gtk_theme_nudge(fresh));
        gsettings = g_settings_new_full(schema, fresh, nullptr);
        value = g_settings_get_string(gsettings, "gtk-theme");
        CHECK(strcmp(value, XD_GTK_THEME_NAME) == 0);
        g_free(value);
        g_object_unref(gsettings);
        /* A dark scheme asks programs for their dark look (the portal's
         * color-scheme), a light one for their default; once each. */
        if (g_settings_schema_has_key(schema, "color-scheme")) {
            gsettings = g_settings_new_full(schema, fresh, nullptr);
            CHECK(xd_gtk_theme_set_color_scheme(fresh, true));
            value = g_settings_get_string(gsettings, "color-scheme");
            CHECK(strcmp(value, "prefer-dark") == 0);
            g_free(value);
            CHECK(!xd_gtk_theme_set_color_scheme(fresh, true));
            CHECK(xd_gtk_theme_set_color_scheme(fresh, false));
            value = g_settings_get_string(gsettings, "color-scheme");
            CHECK(strcmp(value, "default") == 0);
            g_free(value);
            g_object_unref(gsettings);
        }
        g_object_unref(fresh);
        g_settings_schema_unref(schema);
    }
    /* Font smoothing: GTK's settings.ini keys, and GNOME's where it has them. */
    xd_font_smoothing fs = {XD_SMOOTH_SUBPIXEL, XD_SUBPIXEL_BGR, XD_HINT_MEDIUM};
    xd_font_settings_export(config, backend, &fs);
    kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini3, G_KEY_FILE_NONE, nullptr));
    value = g_key_file_get_string(kf, "Settings", "gtk-xft-rgba", nullptr);
    CHECK(g_strcmp0(value, "bgr") == 0);
    g_free(value);
    value = g_key_file_get_string(kf, "Settings", "gtk-xft-hintstyle", nullptr);
    CHECK(g_strcmp0(value, "hintmedium") == 0);
    g_free(value);
    CHECK(g_key_file_get_integer(kf, "Settings", "gtk-xft-antialias", nullptr) == 1);
    g_key_file_free(kf);
    fs = (xd_font_smoothing){XD_SMOOTH_NONE, XD_SUBPIXEL_BGR, XD_HINT_NONE};
    xd_font_settings_export(config, backend, &fs);
    kf = g_key_file_new();
    CHECK(g_key_file_load_from_file(kf, ini4, G_KEY_FILE_NONE, nullptr));
    value = g_key_file_get_string(kf, "Settings", "gtk-xft-rgba", nullptr);
    CHECK(g_strcmp0(value, "none") == 0);
    g_free(value);
    CHECK(g_key_file_get_integer(kf, "Settings", "gtk-xft-antialias", nullptr) == 0);
    CHECK(g_key_file_get_integer(kf, "Settings", "gtk-xft-hinting", nullptr) == 0);
    g_key_file_free(kf);
    schema = src != nullptr
                 ? g_settings_schema_source_lookup(src, "org.gnome.desktop.interface", TRUE)
                 : nullptr;
    if (schema != nullptr && g_settings_schema_has_key(schema, "font-antialiasing")) {
        GSettings *gsettings = g_settings_new_full(schema, backend, nullptr);
        value = g_settings_get_string(gsettings, "font-antialiasing");
        CHECK(strcmp(value, "none") == 0);
        g_free(value);
        value = g_settings_get_string(gsettings, "font-hinting");
        CHECK(strcmp(value, "none") == 0);
        g_free(value);
        value = g_settings_get_string(gsettings, "font-rgba-order");
        CHECK(strcmp(value, "bgr") == 0);
        g_free(value);
        g_object_unref(gsettings);
    }
    if (schema != nullptr)
        g_settings_schema_unref(schema);
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
static bool near0(double value) { return value < 1e-9 && value > -1e-9; }

#if XD_HAVE_LIBARCHIVE
/* A .xdtheme: a zip of `count` files. */
static void write_zip(const char *file, const char *const (*items)[2], size_t count) {
    struct archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    CHECK(archive_write_open_filename(writer, file) == ARCHIVE_OK);
    for (size_t i = 0; i < count; i++) {
        struct archive_entry *member = archive_entry_new();
        archive_entry_set_pathname(member, items[i][0]);
        archive_entry_set_filetype(member, AE_IFREG);
        archive_entry_set_perm(member, 0644);
        size_t len = strlen(items[i][1]);
        archive_entry_set_size(member, (la_int64_t)len);
        CHECK(archive_write_header(writer, member) == ARCHIVE_OK);
        CHECK(archive_write_data(writer, items[i][1], len) == (la_ssize_t)len);
        archive_entry_free(member);
    }
    CHECK(archive_write_close(writer) == ARCHIVE_OK);
    archive_write_free(writer);
}
#endif

static const xd_installed_package *package_at(GPtrArray *list, guint i) {
    return i < list->len ? g_ptr_array_index(list, i) : nullptr;
}

static void test_theme_packages(void) {
    /* [Appearance] ThemePackage, ThemeVariant, ImageScaling, IntegerScaling. */
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(original.theme_package[0] == '\0' && original.theme_variant[0] == '\0');
    CHECK(original.image_scaling == XD_FILTER_COUNT && original.integer_scaling);
    xd_settings_parse(XD_STR("[Appearance]\nThemePackage=Energy Blue\nThemeVariant=royale-normal\n"
                             "ImageScaling=scale2x\nIntegerScaling=false\n"),
                      &original);
    CHECK(strcmp(original.theme_package, "Energy Blue") == 0);
    CHECK(strcmp(original.theme_variant, "royale-normal") == 0);
    CHECK(original.image_scaling == XD_FILTER_SCALE2X && !original.integer_scaling);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "ImageScaling=Scale2x\n") != nullptr);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          strcmp(parsed.theme_package, "Energy Blue") == 0 &&
          parsed.image_scaling == XD_FILTER_SCALE2X && !parsed.integer_scaling);
    xd_strbuf_free(&text);
    /* A name, never a path; filters by name only (bad values keep their
     * defaults); Automatic written so. */
    xd_settings_parse(XD_STR("[Appearance]\nThemePackage=../../etc\nThemeVariant=a/b\n"
                             "ImageScaling=Bicubic\nIntegerScaling=maybe\n"),
                      &original);
    CHECK(original.theme_package[0] == '\0' &&
          original.theme_variant[0] == '\0'); /* the defaults */
    CHECK(original.image_scaling == XD_FILTER_COUNT && original.integer_scaling);
    xd_settings_parse(XD_STR("[Appearance]\nThemePackage=\nImageScaling=automatic\n"), &original);
    CHECK(original.theme_package[0] == '\0' && original.image_scaling == XD_FILTER_COUNT);
    xd_settings_parse(XD_STR("[Appearance]\nThemePackage=.hidden\n"), &original);
    CHECK(original.theme_package[0] == '\0');
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "ImageScaling=Automatic\n") != nullptr);
    xd_strbuf_free(&text);
    CHECK(xd_settings_image_filter(&original, nullptr) == XD_FILTER_SMOOTH);
    original.image_scaling = XD_FILTER_NEAREST;
    CHECK(xd_settings_image_filter(&original, nullptr) == XD_FILTER_NEAREST);

    /* Discovery, in two scratch directories as $XDG_DATA_HOME's and a
     * system one's xeondesktop/themes. Our own little packages. */
    char *root = g_dir_make_tmp("xd-pkgs-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    static const char style[] =
        "<style><sysmetrics><color name=\"ActiveCaption\" value=\"10 20 30\"/></sysmetrics>"
        "<class name=\"Button\"><part name=\"PushButton\" TextColor=\"1 2 3\"/></class></style>";
    put_file(root, "one/Alpha/theme.xml",
             "<xdtheme version=\"1\" name=\"Zulu Style\" base=\"luna\">"
             "<variant id=\"blue\" name=\"Blue\" size=\"Normal\" style=\"styles/blue.xml\"/>"
             "<variant id=\"blue-large\" name=\"Blue\" size=\"Large Fonts\" style=\"styles/blue.xml\"/>"
             "<variant id=\"green\" name=\"Green\" style=\"styles/blue.xml\"/>"
             "<variant id=\"nostyle\" name=\"Nothing\"/>"
             "<scaling filter=\"Integer\"/></xdtheme>");
    put_file(root, "one/Alpha/styles/blue.xml", style);
    /* No variant; no theme.xml; hidden: not packages. */
    put_file(root, "one/Broken/theme.xml", "<xdtheme name=\"Broken\" base=\"classic\"/>");
    put_file(root, "one/Empty/readme.txt", "");
    put_file(root, "one/.Hidden/theme.xml",
             "<xdtheme name=\"Hidden\"><variant id=\"a\" style=\"s.xml\"/></xdtheme>");
    /* Named by its folder when theme.xml names it nothing. */
    put_file(root, "two/Mike/theme.xml",
             "<xdtheme base=\"aero\"><variant id=\"glass\" style=\"s.xml\"/></xdtheme>");
    /* The first directory's wins: one's Alpha over two's. */
    put_file(root, "two/Alpha/theme.xml",
             "<xdtheme name=\"Other Alpha\"><variant id=\"x\" style=\"s.xml\"/></xdtheme>");
#if XD_HAVE_LIBARCHIVE
    /* A zip; and a folder that wins over a zip of its name. */
    char *zip = g_build_filename(root, "one", "Bravo.xdtheme", nullptr);
    static const char *const bravo[][2] = {
        {"theme.xml", "<xdtheme name=\"Bravo\" base=\"whistler\">"
                      "<variant id=\"v\" name=\"Plain\" size=\"Normal\" style=\"styles/v.xml\"/>"
                      "</xdtheme>"},
        {"styles/v.xml", style},
    };
    write_zip(zip, bravo, G_N_ELEMENTS(bravo));
    g_free(zip);
    zip = g_build_filename(root, "two", "Mike.xdtheme", nullptr);
    write_zip(zip, bravo, G_N_ELEMENTS(bravo));
    g_free(zip);
    put_file(root, "one/Mike/theme.xml",
             "<xdtheme name=\"Mike Folder\"><variant id=\"f\" style=\"s.xml\"/></xdtheme>");
    put_file(root, "one/Junk.xdtheme", "not a zip");
#endif
    char *one = g_build_filename(root, "one", nullptr);
    char *two = g_build_filename(root, "two", nullptr);
    const char *dirs[] = {one, two, "/nonexistent", nullptr};
    GPtrArray *list = xd_theme_packages_list(dirs);
#if XD_HAVE_LIBARCHIVE
    static const char *const want[] = {"Bravo", "Mike", "Alpha"}; /* Bravo, Mike Folder, Zulu Style */
#else
    static const char *const want[] = {"Mike", "Alpha"};
#endif
    CHECK(list->len == G_N_ELEMENTS(want));
    for (guint i = 0; i < G_N_ELEMENTS(want); i++)
        CHECK(package_at(list, i) && strcmp(package_at(list, i)->name, want[i]) == 0);
    const xd_installed_package *alpha = package_at(list, G_N_ELEMENTS(want) - 1);
    if (alpha != nullptr) {
        CHECK(strcmp(alpha->info.name, "Zulu Style") == 0 && alpha->info.base == XD_STYLE_LUNA);
        CHECK(alpha->info.has_filter && alpha->info.filter == XD_FILTER_INTEGER);
        /* A variant without a style isn't one; a size defaults to Normal. */
        CHECK(alpha->info.variant_count == 3);
        CHECK(alpha->info.variant_count == 3 && strcmp(alpha->info.variants[1].id, "blue-large") == 0 &&
              strcmp(alpha->info.variants[1].name, "Blue") == 0 &&
              strcmp(alpha->info.variants[1].size, "Large Fonts") == 0 &&
              strcmp(alpha->info.variants[2].size, "Normal") == 0);
        CHECK(g_str_has_prefix(alpha->path, one));
    }
#if XD_HAVE_LIBARCHIVE
    const xd_installed_package *mike = package_at(list, 1);
    CHECK(mike && strcmp(mike->info.name, "Mike Folder") == 0 && !g_str_has_suffix(mike->path, ".xdtheme"));
    const xd_installed_package *bravo_pkg = package_at(list, 0);
    CHECK(bravo_pkg && bravo_pkg->info.base == XD_STYLE_WHISTLER &&
          g_str_has_suffix(bravo_pkg->path, "Bravo.xdtheme") && bravo_pkg->info.variant_count == 1);
#endif
    g_ptr_array_unref(list);
    char *found = xd_theme_package_find(dirs, "Alpha");
    CHECK(found != nullptr && g_str_has_prefix(found, one));
    g_free(found);
    CHECK(xd_theme_package_find(dirs, "../one/Alpha") == nullptr);
    CHECK(xd_theme_package_find(dirs, ".Hidden") == nullptr);
    CHECK(xd_theme_package_find(dirs, "Nope") == nullptr);
    char **search = xd_theme_package_dirs();
    CHECK(g_strv_length(search) >= 2 && g_str_has_suffix(search[0], "/xeondesktop/themes"));
    g_strfreev(search);

    /* The active package, as $XD_THEME_PACKAGE overrides the settings. */
    char *env = g_strdup(g_getenv("XD_THEME_PACKAGE"));
    char *alpha_path = g_build_filename(one, "Alpha", nullptr);
    g_setenv("XD_THEME_PACKAGE", alpha_path, TRUE);
    xd_settings plain;
    xd_settings_defaults(&plain);
    xd_pkg *old = xd_settings_use_package(&plain);
    CHECK(old == nullptr);
    xd_pkg_free(old);
    const xd_pkg *active = xd_settings_active_package();
    CHECK(active != nullptr && strcmp(xd_pkg_name(active), "Zulu Style") == 0 &&
          strcmp(xd_pkg_variant(active), "blue") == 0);
    /* Every theme draws with it: its base style, its [SysMetrics], and
     * Automatic scaling is its own filter. */
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    CHECK(theme.pkg == active && theme.style == XD_STYLE_LUNA &&
          theme.image_filter == XD_FILTER_INTEGER);
    CHECK(theme.scheme.colors[XD_COLOR_ACTIVE_TITLE].r == 10);
    /* The same package again: kept. Another variant: loaded anew. */
    old = xd_settings_use_package(&plain);
    CHECK(old == nullptr && xd_settings_active_package() == active);
    snprintf(plain.theme_variant, sizeof(plain.theme_variant), "%s", "green");
    plain.image_scaling = XD_FILTER_SMOOTH; /* the user's choice wins */
    old = xd_settings_use_package(&plain);
    CHECK(old == active);
    xd_pkg_free(old);
    active = xd_settings_active_package();
    CHECK(active != nullptr && strcmp(xd_pkg_variant(active), "green") == 0);
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    CHECK(theme.pkg == active && theme.image_filter == XD_FILTER_SMOOTH);
    /* A preview with another package, or none, whatever is active. */
    xd_theme_init_package(&theme, XD_STYLE_AERO, &scheme, 1.0, nullptr, XD_FILTER_SMOOTH, true);
    CHECK(theme.pkg == nullptr && theme.style == XD_STYLE_AERO &&
          theme.scheme.colors[XD_COLOR_ACTIVE_TITLE].r == scheme.colors[XD_COLOR_ACTIVE_TITLE].r);
    /* Without the override, the settings' package: none, as named. */
    g_unsetenv("XD_THEME_PACKAGE");
    plain.theme_variant[0] = '\0';
    old = xd_settings_use_package(&plain);
    CHECK(old == active);
    xd_pkg_free(old);
    CHECK(xd_settings_active_package() == nullptr);
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    CHECK(theme.pkg == nullptr && theme.style == XD_STYLE_CLASSIC);
    /* Not installed: none either. */
    snprintf(plain.theme_package, sizeof(plain.theme_package), "%s", "Not Installed Anywhere");
    CHECK(xd_settings_use_package(&plain) == nullptr && xd_settings_active_package() == nullptr);
    if (env != nullptr)
        g_setenv("XD_THEME_PACKAGE", env, TRUE);
    g_free(env);
    g_free(alpha_path);
    g_free(one);
    g_free(two);
    rm_tree(root);
    g_free(root);
}

/* Themes (Display Properties' Themes tab): what a package brings besides
 * its style, unpacked and put into the settings, and Save As... */
static void test_theme_sets(void) {
    char *root = g_dir_make_tmp("xd-themeset-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    char *data = g_build_filename(root, "data", nullptr);
    put_file(root, "pkgs/Sky/theme.xml",
             "<xdtheme version=\"1\" name=\"Sky\" base=\"luna\">"
             "<variant id=\"v\" name=\"Blue\" style=\"styles/v.xml\"/>"
             "<wallpaper file=\"wallpaper/sky.png\" style=\"Tile\"/>"
             "<sounds scheme=\"sounds/scheme.ini\"/><cursors theme=\"cursors\"/></xdtheme>");
    put_file(root, "pkgs/Sky/styles/v.xml", "<style/>");
    put_file(root, "pkgs/Sky/wallpaper/sky.png", "png");
    put_file(root, "pkgs/Sky/sounds/hi.wav", "RIFFhi");
    put_file(root, "pkgs/Sky/sounds/scheme.ini",
             "[Sounds]\nScheme=default\n[Events]\nWindowsLogon=sounds/hi.wav\nMaximize=\n"
             "Question=dialog-question\nMinimize=../../etc/passwd\n");
    put_file(root, "pkgs/Sky/cursors/index.theme", "[Icon Theme]\nName=Sky\n");
    put_file(root, "pkgs/Sky/cursors/cursors/left_ptr", "xcur");
    put_file(root, "pkgs/Plain/theme.xml",
             "<xdtheme name=\"Plain\"><variant id=\"v\" style=\"s.xml\"/>"
             "<wallpaper file=\"../x.png\"/></xdtheme>");
    char *pkgs = g_build_filename(root, "pkgs", nullptr);
    const char *dirs[] = {pkgs, nullptr};
    GPtrArray *list = xd_theme_packages_list(dirs);
    CHECK(list->len == 2);
    const xd_installed_package *plain = package_at(list, 0), *sky = package_at(list, 1);
    if (plain == nullptr || sky == nullptr) {
        g_ptr_array_unref(list);
        return;
    }
    /* A path out of the package is none: Plain brings nothing more. */
    CHECK(!xd_theme_set_has_extras(&plain->info) && xd_theme_set_has_extras(&sky->info));
    CHECK(strcmp(sky->info.wallpaper_style, "tile") == 0);

    xd_settings original;
    xd_settings_defaults(&original);
    snprintf(original.theme_package, sizeof(original.theme_package), "Sky");
    xd_sound_scheme sounds;
    xd_sound_scheme_init(&sounds, XD_SOUNDS_NONE);
    bool sounds_set = false;
    GError *error = nullptr;
    /* Worked out without unpacking: nothing written. */
    xd_settings probe = original;
    CHECK(xd_theme_set_extras(sky, data, false, &probe, &sounds, &sounds_set, &error));
    CHECK(!g_file_test(data, G_FILE_TEST_EXISTS));
    CHECK(xd_theme_set_extras(sky, data, true, &original, &sounds, &sounds_set, &error));
    CHECK(error == nullptr && sounds_set);
    char *files = g_build_filename(data, "xeondesktop", "theme-files", "Sky", nullptr);
    char *wall = g_build_filename(files, "wallpaper", "sky.png", nullptr);
    char *hi = g_build_filename(files, "sounds", "hi.wav", nullptr);
    char *cur = g_build_filename(data, "icons", "xdtheme-Sky", "cursors", "left_ptr", nullptr);
    char *index = g_build_filename(data, "icons", "xdtheme-Sky", "index.theme", nullptr);
    CHECK(strcmp(original.wallpaper, wall) == 0 && original.wallpaper_mode == XD_WALLPAPER_TILE);
    CHECK(strcmp(probe.wallpaper, wall) == 0);
    CHECK(strcmp(original.mouse.cursor_theme, "xdtheme-Sky") == 0);
    CHECK(g_file_test(wall, G_FILE_TEST_IS_REGULAR) && g_file_test(hi, G_FILE_TEST_IS_REGULAR));
    CHECK(g_file_test(cur, G_FILE_TEST_IS_REGULAR) && g_file_test(index, G_FILE_TEST_IS_REGULAR));
    /* Its sounds, its silences and theme names; a bad value keeps the
     * scheme's (Windows Default: the theme's base). */
    CHECK(sounds.base == XD_SOUNDS_DEFAULT && strcmp(sounds.sounds[XD_SOUND_LOGON], hi) == 0);
    CHECK(strcmp(sounds.sounds[XD_SOUND_MAXIMIZE], "") == 0);
    CHECK(strcmp(sounds.sounds[XD_SOUND_QUESTION], "dialog-question") == 0);
    CHECK(strcmp(sounds.sounds[XD_SOUND_MINIMIZE], "") == 0); /* Windows Default's */
    CHECK(strcmp(sounds.sounds[XD_SOUND_EXCLAMATION], "dialog-warning") == 0);
    CHECK(xd_theme_set_matches(sky, data, &original, &sounds));
    xd_settings changed = original;
    changed.wallpaper_mode = XD_WALLPAPER_CENTER;
    CHECK(!xd_theme_set_matches(sky, data, &changed, &sounds));
    changed = original;
    changed.mouse.cursor_theme[0] = '\0';
    CHECK(!xd_theme_set_matches(sky, data, &changed, &sounds));
    CHECK(xd_sound_scheme_set(&sounds, XD_SOUND_MAXIMIZE, "bell"));
    CHECK(!xd_theme_set_matches(sky, data, &original, &sounds));
    CHECK(xd_sound_scheme_set(&sounds, XD_SOUND_MAXIMIZE, ""));
    char name[64];
    CHECK(!xd_theme_set_cursor_theme("a-name-far-too-long-for-a-cursor-theme-directory-name-of-sixty-four",
                                     name, sizeof(name)));

#if XD_HAVE_LIBARCHIVE
    /* Save As...: Sky's style and the current wallpaper, sounds and
     * cursors, as a zip; read back, and chosen in its turn. */
    char *saved = g_build_filename(pkgs, "Mine.xdtheme", nullptr);
    char *icons = g_build_filename(data, "icons", nullptr);
    const char *icon_dirs[] = {icons, nullptr};
    CHECK(xd_theme_set_save(saved, "Mine", &original, &sounds, sky->path, "v", icon_dirs, &error));
    CHECK(error == nullptr);
    g_clear_error(&error);
    GBytes *xml = xd_pkg_read_member(saved, "theme.xml", nullptr);
    CHECK(xml != nullptr && strstr(g_bytes_get_data(xml, nullptr), "name=\"Mine\"") != nullptr);
    if (xml != nullptr)
        g_bytes_unref(xml);
    GBytes *scheme = xd_pkg_read_member(saved, "sounds/scheme.ini", nullptr);
    CHECK(scheme != nullptr &&
          strstr(g_bytes_get_data(scheme, nullptr), "WindowsLogon=sounds/hi.wav") != nullptr);
    if (scheme != nullptr)
        g_bytes_unref(scheme);
    GPtrArray *again = xd_theme_packages_list(dirs);
    const xd_installed_package *mine = package_at(again, 0);
    CHECK(mine != nullptr && strcmp(mine->name, "Mine") == 0);
    if (mine != nullptr) {
        CHECK(mine->info.variant_count == 1 && strcmp(mine->info.variants[0].id, "v") == 0);
        CHECK(g_strcmp0(mine->info.wallpaper, "wallpaper/sky.png") == 0 &&
              g_strcmp0(mine->info.wallpaper_style, "tile") == 0 &&
              g_strcmp0(mine->info.cursors, "cursors") == 0);
        xd_settings modified = original;
        xd_sound_scheme other;
        xd_sound_scheme_init(&other, XD_SOUNDS_NONE);
        snprintf(modified.theme_package, sizeof(modified.theme_package), "Mine");
        CHECK(xd_theme_set_extras(mine, data, true, &modified, &other, &sounds_set, nullptr));
        char *hi2 = g_build_filename(data, "xeondesktop", "theme-files", "Mine", "sounds",
                                     "hi.wav", nullptr);
        CHECK(strcmp(other.sounds[XD_SOUND_LOGON], hi2) == 0 &&
              g_file_test(hi2, G_FILE_TEST_IS_REGULAR));
        CHECK(strcmp(modified.mouse.cursor_theme, "xdtheme-Mine") == 0);
        CHECK(xd_theme_set_matches(mine, data, &modified, &other));
        g_free(hi2);
        xd_sound_scheme_free(&other);
    }
    g_ptr_array_unref(again);
    /* A missing member: an error, nothing unpacked. */
    char *nowhere = g_build_filename(root, "nowhere", nullptr);
    CHECK(!xd_pkg_extract(saved, "wallpaper/none.png", nowhere, &error) && error != nullptr);
    g_clear_error(&error);
    CHECK(!xd_pkg_extract(saved, "../theme.xml", nowhere, &error) && error != nullptr);
    g_clear_error(&error);
    CHECK(!g_file_test(nowhere, G_FILE_TEST_EXISTS));
    g_free(nowhere);
    g_free(icons);
    g_free(saved);
#endif
    xd_sound_scheme_free(&sounds);
    g_free(index);
    g_free(cur);
    g_free(hi);
    g_free(wall);
    g_free(files);
    g_ptr_array_unref(list);
    g_free(pkgs);
    g_free(data);
    rm_tree(root);
    g_free(root);
}

static void test_glass_color(void) {
    xd_settings settings;
    xd_settings_defaults(&settings);
    CHECK(settings.glass_color == XD_GLASS_DEFAULT && settings.glass_transparency);
    xd_settings_parse(XD_STR("[Appearance]\nGlassColor=a8ce0f0f\nGlassTransparency=false\n"),
                      &settings);
    CHECK(settings.glass_color == 0xA8CE0F0Fu && !settings.glass_transparency);
    /* Not 8 hex digits: ignored. */
    xd_settings_parse(XD_STR("[Appearance]\nGlassColor=0xA8CE0F\n"), &settings);
    CHECK(settings.glass_color == XD_GLASS_DEFAULT);
    xd_settings_parse(XD_STR("[Appearance]\nGlassColor=-1234567\n"), &settings);
    CHECK(settings.glass_color == XD_GLASS_DEFAULT);
    settings.glass_color = 0x54FCFCFCu;
    settings.glass_transparency = false;
    xd_strbuf text = {0};
    xd_settings_serialize(&settings, &text);
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
        xd_rgba color = xd_rgba_hex(rgbs[i]);
        xd_rgba back_c = xd_rgba_from_hsv(xd_rgba_to_hsv(color), 0.5);
        CHECK(near0(back_c.r - color.r) && near0(back_c.g - color.g) && near0(back_c.b - color.b) &&
              back_c.a == 0.5);
    }
    CHECK(near0(xd_rgba_to_hsv(xd_rgba_hex(0x0000FF)).h - 240));

    /* Sixteen named swatches, Sky first and the default. */
    CHECK(strcmp(xd_glass_swatches[0].name, "Sky") == 0 &&
          xd_glass_swatches[0].argb == XD_GLASS_DEFAULT);
    CHECK(strcmp(xd_glass_swatches[XD_GLASS_SWATCH_COUNT - 1].name, "Frost") == 0);

    /* Opaque glass: the colour in light grey by its intensity. */
    xd_scheme_init_default(&scheme);
    scheme.glass_color = 0x80FF0000u;
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_AERO, &scheme, 1.0);
    theme.glass = false;
    xd_glass_colors glass;
    xd_theme_glass_colors(&theme, &glass);
    double k = 0x80 / 255.0, grey = 0xEB / 255.0;
    CHECK(!glass.translucent && glass.frame.a == 1.0 &&
          near0(glass.frame.r - (grey + (1 - grey) * k)) && near0(glass.frame.g - grey * (1 - k)));
    /* Translucent with blur and transparency on: more intensity, more colour. */
    theme.glass = true;
    xd_theme_glass_colors(&theme, &glass);
    CHECK(glass.translucent && glass.frame.a < 1.0 && glass.frame.r == 1.0 && glass.frame.g == 0.0);
    double low = glass.frame.a;
    theme.scheme.glass_color = 0xC0FF0000u;
    xd_theme_glass_colors(&theme, &glass);
    CHECK(glass.frame.a > low && glass.taskbar.a > 0 && glass.taskbar.r > glass.taskbar.g);
    /* Transparency off: opaque even with blur. */
    theme.scheme.glass_opaque = true;
    xd_theme_glass_colors(&theme, &glass);
    CHECK(!glass.translucent && glass.frame.a == 1.0 && glass.taskbar.a == 1.0 &&
          glass.menu.a == 1.0);

    /* The GTK theme's title bars follow the colour. */
    xd_strbuf sky = {0}, ruby = {0};
    xd_scheme_init_default(&scheme);
    xd_theme_init(&theme, XD_STYLE_AERO, &scheme, 1.0);
    xd_theme_gtk_css(&theme, &sky);
    theme.scheme.glass_color = 0xA8CE0F0Fu;
    xd_theme_gtk_css(&theme, &ruby);
    CHECK(!xd_str_eq(xd_strbuf_view(&sky), xd_strbuf_view(&ruby)));
    xd_toolkit_colors tc;
    xd_theme_toolkit_colors(&theme, &tc);
    CHECK(tc.title_bg.r > tc.title_bg.g + 0.4 && tc.title_bg2.r > tc.title_bg2.b);
    xd_strbuf_free(&sky);
    xd_strbuf_free(&ruby);
}

static void test_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(original.style == XD_STYLE_CLASSIC && original.wallpaper_mode == XD_WALLPAPER_FILL);

    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nScheme=windows-classic\n"
                             "FontSize=large\nFontSmoothing=cleartype\nFuture=ignored\n"
                             "[Desktop]\nWallpaper=/w/p.jpg\nWallpaperMode=tile\n"
                             "[Effects]\nLargeIcons=true\nMenuTransition=Scroll\n"
                             "FullWindowDrag=false\n[ScreenSaver]\nWait=7\n"
                             "[SomethingNew]\nKey=v\n"),
                      &original);
    CHECK(original.style == XD_STYLE_LUNA && strcmp(original.scheme, "windows-classic") == 0);
    /* An older file's Windows name for subpixel smoothing still reads. */
    CHECK(original.font_size == XD_FONT_LARGE &&
          original.font_smoothing.antialias == XD_SMOOTH_SUBPIXEL);
    CHECK(original.font_smoothing.order == XD_SUBPIXEL_RGB &&
          original.font_smoothing.hinting == XD_HINT_SLIGHT);
    CHECK(strcmp(original.wallpaper, "/w/p.jpg") == 0 &&
          original.wallpaper_mode == XD_WALLPAPER_TILE);
    CHECK(original.large_icons && original.screensaver_wait == 7);
    CHECK(original.menu_transition == XD_TRANSITION_SCROLL && !original.full_window_drag);

    /* Serialize -> parse round trip. */
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed));
    parsed.large_icons = false;
    CHECK(!xd_settings_equal(&original, &parsed));
    parsed.large_icons = true;
    parsed.menu_transition = XD_TRANSITION_FADE;
    CHECK(!xd_settings_equal(&original, &parsed));
    xd_strbuf_free(&text);

    /* Font smoothing's three keys, written with their own names. */
    original.font_smoothing =
        (xd_font_smoothing){XD_SMOOTH_SUBPIXEL, XD_SUBPIXEL_VBGR, XD_HINT_FULL};
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "FontSmoothing=subpixel") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&text), "FontSubpixelOrder=vbgr") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&text), "FontHinting=full") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&text), "cleartype") == nullptr);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(parsed.font_smoothing.antialias == XD_SMOOTH_SUBPIXEL &&
          parsed.font_smoothing.order == XD_SUBPIXEL_VBGR &&
          parsed.font_smoothing.hinting == XD_HINT_FULL);
    parsed.font_smoothing.hinting = XD_HINT_NONE;
    CHECK(!xd_settings_equal(&original, &parsed));
    xd_strbuf_free(&text);
    xd_settings_parse(XD_STR("[Appearance]\nFontSmoothing=standard\nFontHinting=bogus\n"), &parsed);
    CHECK(parsed.font_smoothing.antialias == XD_SMOOTH_GRAYSCALE &&
          parsed.font_smoothing.hinting == XD_HINT_SLIGHT);

    /* Bad values keep defaults; a scheme can't be a path. */
    xd_settings_parse(XD_STR("[Appearance]\nStyle=bogus\nScheme=../../etc/x\n"
                             "[Desktop]\nWallpaper=relative.jpg\nWallpaperMode=x\n"
                             "[Effects]\nMenuTransition=wipe\n[ScreenSaver]\nWait=0\n"),
                      &original);
    CHECK(original.style == XD_STYLE_CLASSIC && original.scheme[0] == '\0' &&
          original.wallpaper[0] == '\0');
    CHECK(original.menu_transition == XD_TRANSITION_FADE && original.full_window_drag);
    CHECK(original.wallpaper_mode == XD_WALLPAPER_FILL && original.screensaver_wait == 15);
    /* A syntax error leaves everything at defaults. */
    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nbroken line\n"), &original);
    CHECK(original.style == XD_STYLE_CLASSIC);

    /* Font size scales fonts and caption metrics. */
    xd_settings_defaults(&original);
    xd_scheme normal;
    xd_settings_scheme(&original, &normal);
    original.font_size = XD_FONT_EXTRA_LARGE;
    xd_scheme xl;
    xd_settings_scheme(&original, &xl);
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
    xd_settings_defaults(&original);
    CHECK(original.start_large_icons && original.start_programs == 6 &&
          original.start_highlight_new);
    CHECK(!original.quick_large && !original.quick_show_text && !original.quick_show_title);
    xd_settings_parse(XD_STR("[StartMenu]\nLargeIcons=false\nPrograms=12\n"
                             "HighlightNewPrograms=false\n[Taskbar]\nQuickLaunchLargeIcons=true\n"
                             "QuickLaunchShowText=true\nQuickLaunchShowTitle=true\n"),
                      &original);
    CHECK(!original.start_large_icons && original.start_programs == 12 &&
          !original.start_highlight_new);
    CHECK(original.quick_large && original.quick_show_text && original.quick_show_title);
    xd_settings_serialize(&original, &text);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && parsed.start_programs == 12 &&
          parsed.quick_show_title);
    xd_strbuf_free(&text);
    xd_settings_parse(XD_STR("[StartMenu]\nPrograms=31\n"), &original);
    CHECK(original.start_programs == 6);
}

static void test_saver_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    const xd_saver_settings *sv = &original.savers;
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
                      &original);
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
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          memcmp(&original.savers, &parsed.savers, sizeof(original.savers)) == 0);
    xd_strbuf_free(&text);

    /* Out of range or malformed values keep the defaults. */
    xd_settings_parse(XD_STR("[ScreenSaver.Marquee]\nSpeed=11\nTextColor=256 0 0\n"
                             "BackgroundColor=1 2\nPosition=left\nText=a\tb\n"
                             "[ScreenSaver.Starfield]\nDensity=5\n"
                             "[ScreenSaver.Mystify]\nPolygon1Lines=16\nPolygon3Active=false\n"
                             "[ScreenSaver.Beziers]\nCurves=0\nLines=51\n"
                             "[ScreenSaver.Pipes]\nJoints=bogus\nPipes=many\n"
                             "[ScreenSaver.Text3D]\nRotation=tumble\nSize=-1\n"
                             "[ScreenSaver.Maze]\nSpeed=11\nPixelated=sometimes\n"
                             "[ScreenSaver.FlowerBox]\nShape=sphere\nBloom=-2\n"),
                      &original);
    xd_saver_settings def;
    xd_saver_settings_defaults(&def);
    CHECK(memcmp(&original.savers, &def, sizeof(def)) == 0);
}

/* Renders `frames` frames of a saver from `seed` into a small image. */
static cairo_surface_t *saver_frames(const char *id, const xd_saver_settings *saver_settings,
                                     uint64_t seed, int frames, double dt) {
    cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 160, 120);
    cairo_t *cairo = cairo_create(img);
    xd_saver *sv = xd_saver_create_seeded(id, saver_settings, 160, 120, 0.5, seed);
    for (int i = 0; i < frames; i++)
        xd_saver_frame(sv, cairo, dt);
    xd_saver_destroy(sv);
    cairo_destroy(cairo);
    cairo_surface_flush(img);
    return img;
}

static bool same_pixels(cairo_surface_t *left, cairo_surface_t *right) {
    size_t count =
        (size_t)cairo_image_surface_get_stride(left) * (size_t)cairo_image_surface_get_height(left);
    return memcmp(cairo_image_surface_get_data(left), cairo_image_surface_get_data(right), count) ==
           0;
}

static void test_savers(void) {
    const xd_saver_info *list = nullptr;
    size_t count = xd_savers(&list);
    CHECK(count == 10 && strcmp(list[0].name, "3D Flower Box") == 0 &&
          strcmp(list[1].id, "maze") == 0 && strcmp(list[3].id, "text3d") == 0);
    CHECK(xd_saver_find("pipes") != nullptr && xd_saver_find("nope") == nullptr);
    CHECK(!xd_saver_animated("blank") && !xd_saver_has_settings("blank"));
    CHECK(xd_saver_has_settings("marquee") && xd_saver_create("nope", nullptr, 10, 10, 1) == nullptr);

    xd_settings st;
    xd_settings_defaults(&st);
    for (size_t i = 0; i < count; i++) {
        const char *id = list[i].id;
        /* The same seed and frame times give the same picture... */
        cairo_surface_t *first = saver_frames(id, &st.savers, 7, 40, 1.0 / 30);
        cairo_surface_t *second = saver_frames(id, &st.savers, 7, 40, 1.0 / 30);
        bool same = same_pixels(first, second);
        CHECK(same);
        if (!same)
            fprintf(stderr, "  saver %s isn't deterministic\n", id);
        /* ...and the picture moves on (Blank Screen's stays black). */
        cairo_surface_t *later = saver_frames(id, &st.savers, 7, 41, 1.0 / 30);
        bool moved = !same_pixels(first, later);
        CHECK(moved == xd_saver_animated(id));
        if (moved != xd_saver_animated(id))
            fprintf(stderr, "  saver %s: moved %d\n", id, moved);
        cairo_surface_destroy(first);
        cairo_surface_destroy(second);
        cairo_surface_destroy(later);
    }
    /* Where the random numbers show, another seed is another picture. */
    static const char *const random_ones[] = {"pipes",     "maze",   "flowerbox", "beziers",
                                              "mystify",   "starfield", "flying"};
    for (size_t i = 0; i < XD_ARRAY_LEN(random_ones); i++) {
        cairo_surface_t *first = saver_frames(random_ones[i], &st.savers, 7, 20, 1.0 / 30);
        cairo_surface_t *second = saver_frames(random_ones[i], &st.savers, 8, 20, 1.0 / 30);
        CHECK(!same_pixels(first, second));
        cairo_surface_destroy(first);
        cairo_surface_destroy(second);
    }

    /* Every option draws: 3D Pipes filling the screen several times over,
     * Mystify keeping its trails, 3D Text in each rotation and surface. */
    xd_saver_settings saver_settings = st.savers;
    saver_settings.pipes.multiple = true;
    saver_settings.pipes.joints = XD_PIPES_CYCLE;
    cairo_surface_destroy(saver_frames("pipes", &saver_settings, 3, 600, 0.25));
    saver_settings.mystify.clear_screen = false;
    saver_settings.mystify.shapes[0].two_colors = true;
    cairo_surface_t *m1 = saver_frames("mystify", &saver_settings, 3, 30, 1.0 / 30);
    cairo_surface_t *m2 = saver_frames("mystify", &saver_settings, 3, 34, 1.0 / 30);
    CHECK(!same_pixels(m1, m2));
    cairo_surface_destroy(m1);
    cairo_surface_destroy(m2);
    for (int rotation = 0; rotation < XD_TEXT3D_ROTATION_COUNT; rotation++) {
        saver_settings.text3d.rotation = (xd_text3d_rotation)rotation;
        saver_settings.text3d.surface = rotation % 2 == 0 ? XD_TEXT3D_SOLID : XD_TEXT3D_REFLECTION;
        cairo_surface_t *first = saver_frames("text3d", &saver_settings, 5, 15, 0.1);
        cairo_surface_t *second = saver_frames("text3d", &saver_settings, 5, 15, 0.1);
        CHECK(same_pixels(first, second));
        cairo_surface_destroy(first);
        cairo_surface_destroy(second);
    }
    saver_settings.text3d.show_time = true;
    cairo_surface_destroy(saver_frames("text3d", &saver_settings, 5, 3, 0.1));
    saver_settings.marquee.text[0] = '\0';
    cairo_surface_destroy(saver_frames("marquee", &saver_settings, 5, 3, 0.1));

    /* 3D Maze: walked to the exit and into the next maze (a minute of
     * walking at full speed through the smallest), smooth and pixelated;
     * 3D Flower Box in each shape. */
    saver_settings.maze.speed = XD_SAVER_SLIDER_MAX;
    saver_settings.maze.size = 0;
    for (int pixelated = 0; pixelated < 2; pixelated++) {
        saver_settings.maze.pixelated = pixelated == 1;
        cairo_surface_destroy(saver_frames("maze", &saver_settings, 4, 600, 0.1));
    }
    for (int shape = 0; shape < XD_FLOWER_SHAPE_COUNT; shape++) {
        saver_settings.flower.shape = (xd_flower_shape)shape;
        saver_settings.flower.cycle = shape % 2 == 0;
        cairo_surface_destroy(saver_frames("flowerbox", &saver_settings, 4, 60, 0.1));
    }
}

static void test_desktop_icon_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    const xd_desktop_icon_settings *di = &original.desktop_icons;
    CHECK(di->documents && di->computer && di->network && !di->browser && di->show);
    CHECK(!di->auto_arrange && di->align_to_grid && xd_desk_icon_px(&original) == 32);
    /* XP's "Use large icons" makes the (medium) desktop icons 48. */
    original.large_icons = true;
    CHECK(xd_desk_icon_px(&original) == 48);
    original.desktop_icons.size = XD_DESK_ICONS_SMALL;
    CHECK(xd_desk_icon_px(&original) == 16);

    xd_settings_parse(XD_STR("[DesktopIcons]\nMyComputer=false\nWebBrowser=true\n"
                             "AutoArrange=true\nAlignToGrid=false\nSize=large\nShow=false\n"
                             "MyComputerIcon=/i/pc.png\nRecycleBinFullIcon=relative.png\n"),
                      &original);
    CHECK(!di->computer && di->browser && di->auto_arrange && !di->align_to_grid && !di->show);
    CHECK(di->size == XD_DESK_ICONS_LARGE && xd_desk_icon_px(&original) == 48);
    CHECK(strcmp(di->icons[XD_DESK_ICON_COMPUTER], "/i/pc.png") == 0);
    CHECK(di->icons[XD_DESK_ICON_TRASH_FULL][0] == '\0'); /* only absolute paths */
    CHECK(strcmp(xd_desk_icon_default(XD_DESK_ICON_TRASH_EMPTY), "user-trash") == 0);

    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && parsed.desktop_icons.auto_arrange &&
          strcmp(parsed.desktop_icons.icons[XD_DESK_ICON_COMPUTER], "/i/pc.png") == 0);
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
    xd_settings original;
    xd_settings_defaults(&original);
    /* XP's defaults: the volume and power icons shown, a password on resume. */
    CHECK(original.volume_icon && original.battery_icon && original.resume_password);
    CHECK(original.power_scheme == XD_POWER_HOME && original.standby == 0 &&
          original.hibernate == 0);
    xd_settings_parse(XD_STR("[Sound]\nVolumeIcon=false\n"
                             "[Power]\nScheme=portable\nStandby=20\nHibernate=180\n"
                             "AlwaysShowIcon=false\nPasswordOnResume=false\n"),
                      &original);
    CHECK(!original.volume_icon && original.power_scheme == XD_POWER_PORTABLE);
    CHECK(original.standby == 20 && original.hibernate == 180 && !original.battery_icon &&
          !original.resume_password);
    CHECK(strcmp(xd_power_scheme_name(original.power_scheme), "portable") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && parsed.standby == 20 && !parsed.volume_icon);
    xd_strbuf_free(&text);
    /* The battery column and a scheme of the user's own. */
    xd_settings_parse(XD_STR("[Power]\nMonitorOffOnBattery=5\nStandbyOnBattery=10\n"
                             "HibernateOnBattery=45\nCustomScheme=My Lab\n"),
                      &original);
    CHECK(original.monitor_off_battery == 5 && original.standby_battery == 10 &&
          original.hibernate_battery == 45);
    CHECK(strcmp(original.power_scheme_custom, "My Lab") == 0 && original.standby == 0);
    xd_settings_serialize(&original, &text);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && parsed.hibernate_battery == 45);
    CHECK(strcmp(parsed.power_scheme_custom, "My Lab") == 0);
    xd_strbuf_free(&text);
    /* Out of range or unknown: the defaults stay. */
    xd_settings_parse(XD_STR("[Power]\nScheme=turbo\nStandby=-5\nHibernate=99999\n"), &original);
    CHECK(original.power_scheme == XD_POWER_HOME && original.standby == 0 &&
          original.hibernate == 0);
}

/* Power Options' Power buttons: [Power] LidAction and the buttons' keys,
 * what the shell inhibits in logind, and what it does. */
static void test_power_buttons(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    char what[128];
    /* Unset: logind acts, nothing is inhibited or written. */
    for (size_t button = 0; button < XD_POWER_BUTTON_COUNT; button++)
        CHECK(original.power_buttons[button] == XD_BUTTON_LOGIND);
    xd_power_buttons_inhibit_what(&original, what, sizeof(what));
    CHECK(what[0] == '\0');
    CHECK(xd_power_button_action(&original, XD_BUTTON_POWER, false) == XD_BUTTON_NOTHING);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "LidAction") == nullptr);
    CHECK(strstr(xd_strbuf_cstr(&text), "ButtonAction") == nullptr);
    xd_strbuf_free(&text);

    xd_settings_parse(XD_STR("[Power]\nLidAction=lock\nPowerButtonAction=ask\n"
                             "SleepButtonAction=reboot\n"),
                      &original);
    CHECK(original.power_buttons[XD_BUTTON_LID] == XD_BUTTON_LOCK);
    CHECK(original.power_buttons[XD_BUTTON_POWER] == XD_BUTTON_ASK);
    CHECK(original.power_buttons[XD_BUTTON_SLEEP] == XD_BUTTON_LOGIND); /* unknown: unset */
    xd_power_buttons_inhibit_what(&original, what, sizeof(what));
    CHECK(strcmp(what, "handle-lid-switch:handle-power-key") == 0);
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "PowerButtonAction=ask") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&text), "SleepButtonAction") == nullptr);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          parsed.power_buttons[XD_BUTTON_LID] == XD_BUTTON_LOCK);
    xd_strbuf_free(&text);

    /* The sleep button is logind's suspend and hibernate keys. */
    original.power_buttons[XD_BUTTON_SLEEP] = XD_BUTTON_HIBERNATE;
    xd_power_buttons_inhibit_what(&original, what, sizeof(what));
    CHECK(strcmp(what, "handle-lid-switch:handle-power-key:handle-suspend-key:"
                       "handle-hibernate-key") == 0);
    CHECK(xd_power_button_action(&original, XD_BUTTON_SLEEP, true) == XD_BUTTON_HIBERNATE);
    /* Docked or with another monitor, closing the lid does nothing, as
     * logind's HandleLidSwitchDocked=ignore; the lid never asks. */
    CHECK(xd_power_button_action(&original, XD_BUTTON_LID, false) == XD_BUTTON_LOCK);
    CHECK(xd_power_button_action(&original, XD_BUTTON_LID, true) == XD_BUTTON_NOTHING);
    original.power_buttons[XD_BUTTON_LID] = XD_BUTTON_ASK;
    CHECK(xd_power_button_action(&original, XD_BUTTON_LID, false) == XD_BUTTON_NOTHING);
    CHECK(xd_power_button_action(&original, XD_BUTTON_POWER, true) == XD_BUTTON_ASK);

    /* Names, and logind's values in Power Options' terms. */
    xd_button_action act;
    CHECK(xd_button_action_from_name("standby", &act) && act == XD_BUTTON_STANDBY);
    CHECK(!xd_button_action_from_name("reboot", &act));
    CHECK(strcmp(xd_button_action_name(XD_BUTTON_SHUTDOWN), "shutdown") == 0);
    CHECK(xd_button_action_from_logind("suspend", &act) && act == XD_BUTTON_STANDBY);
    CHECK(xd_button_action_from_logind("poweroff", &act) && act == XD_BUTTON_SHUTDOWN);
    CHECK(xd_button_action_from_logind("ignore", &act) && act == XD_BUTTON_NOTHING);
    CHECK(!xd_button_action_from_logind("suspend-then-hibernate", &act));
    CHECK(!xd_button_action_from_logind(nullptr, &act));

    /* Set back to logind's while another applet saved meanwhile: merging
     * removes the key. */
    xd_settings base = original, pending = original, now;
    xd_settings_defaults(&now);
    now.power_buttons[XD_BUTTON_POWER] = XD_BUTTON_ASK;
    pending.power_buttons[XD_BUTTON_POWER] = XD_BUTTON_LOGIND;
    xd_settings_merge(&base, &pending, &now);
    CHECK(now.power_buttons[XD_BUTTON_POWER] == XD_BUTTON_LOGIND);
}

/* Power Options' Alarms: the keys, and when each alarm goes off. */
static void test_battery_alarms(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    /* XP's levels, sound and message on, no action. */
    CHECK(original.low_alarm.on && original.low_alarm.percent == 10 && original.low_alarm.sound &&
          original.low_alarm.message && original.low_alarm.action == XD_ALARM_NOTHING);
    CHECK(original.critical_alarm.on && original.critical_alarm.percent == 3 &&
          original.critical_alarm.action == XD_ALARM_NOTHING);
    xd_settings_parse(XD_STR("[Power]\nLowAlarmLevel=25\nLowAlarmSound=false\n"
                             "CriticalAlarmAction=hibernate\nCriticalAlarmMessage=false\n"
                             "LowAlarmAction=reboot\nCriticalAlarmLevel=101\n"),
                      &original);
    CHECK(original.low_alarm.percent == 25 && !original.low_alarm.sound &&
          original.low_alarm.message);
    /* An unknown action and a level out of range: the defaults. */
    CHECK(original.low_alarm.action == XD_ALARM_NOTHING && original.critical_alarm.percent == 3);
    CHECK(original.critical_alarm.action == XD_ALARM_HIBERNATE && !original.critical_alarm.message);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "CriticalAlarmAction=hibernate") != nullptr);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && parsed.low_alarm.percent == 25);
    xd_strbuf_free(&text);

    /* Discharging from 30%: low at 25, critical at 3, each once. */
    xd_alarm_done done = {0};
    CHECK(xd_battery_alarm_check(&original, &done, 30, true) == XD_ALARM_FIRED_NONE);
    CHECK(xd_battery_alarm_check(&original, &done, 25, true) == XD_ALARM_FIRED_LOW);
    CHECK(xd_battery_alarm_check(&original, &done, 20, true) == XD_ALARM_FIRED_NONE);
    CHECK(xd_battery_alarm_check(&original, &done, 3, true) == XD_ALARM_FIRED_CRITICAL);
    CHECK(xd_battery_alarm_check(&original, &done, 1, true) == XD_ALARM_FIRED_NONE);
    /* Plugged in, then unplugged again lower down: they start over, and a
     * fall straight past both is only the critical one. */
    CHECK(xd_battery_alarm_check(&original, &done, 2, false) == XD_ALARM_FIRED_NONE);
    CHECK(xd_battery_alarm_check(&original, &done, 2, true) == XD_ALARM_FIRED_CRITICAL);
    CHECK(xd_battery_alarm_check(&original, &done, 2, true) == XD_ALARM_FIRED_NONE);
    /* An alarm turned off never goes off. */
    original.critical_alarm.on = false;
    done = (xd_alarm_done){0};
    CHECK(xd_battery_alarm_check(&original, &done, 2, true) == XD_ALARM_FIRED_LOW);
    CHECK(xd_battery_alarm_check(&original, &done, 1, true) == XD_ALARM_FIRED_NONE);
}

/* Power Options' UPS tab: [UPS], NUT's status flags, the tab's values,
 * and when the shell notifies and raises the critical alarm. */
static void test_ups(void) {
    xd_settings settings;
    xd_settings_defaults(&settings);
    xd_ups_settings *ups = &settings.ups;
    /* XP's: notifications after 5 s, then every 2 minutes; no UPS. */
    CHECK(ups->device[0] == '\0' && ups->notify && ups->notify_delay == 5 &&
          ups->notify_interval == 120 && !ups->critical_after && ups->critical_minutes == 2 &&
          ups->action == XD_UPS_SHUTDOWN);
    xd_settings_parse(XD_STR("[UPS]\nDevice=nut:myups@server:3494\nFirstNotification=30\n"
                             "NotificationInterval=1\nCriticalAfterMinutes=true\n"
                             "CriticalMinutes=10\nRunProgram=true\nProgram=save-all --now\n"
                             "CriticalAction=hibernate\n"),
                      &settings);
    CHECK(strcmp(ups->device, "nut:myups@server:3494") == 0 && ups->notify_delay == 30);
    /* Out of range: kept. */
    CHECK(ups->notify_interval == 120);
    CHECK(ups->critical_after && ups->critical_minutes == 10 && ups->run_program &&
          strcmp(ups->program, "save-all --now") == 0 && ups->action == XD_UPS_HIBERNATE);
    xd_strbuf text = {0};
    xd_settings_serialize(&settings, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "CriticalAction=hibernate") != nullptr);
    xd_settings again;
    xd_settings_parse(xd_strbuf_view(&text), &again);
    CHECK(xd_settings_equal(&settings, &again) && strcmp(again.ups.program, ups->program) == 0);
    xd_strbuf_free(&text);

    const char *address = nullptr;
    CHECK(xd_ups_get_device_kind(ups->device, &address) == XD_UPS_DEVICE_NUT &&
          strcmp(address, "myups@server:3494") == 0);
    CHECK(xd_ups_get_device_kind("upower:/org/freedesktop/UPower/devices/ups_hiddev0", &address) ==
              XD_UPS_DEVICE_UPOWER &&
          strcmp(address, "/org/freedesktop/UPower/devices/ups_hiddev0") == 0);
    CHECK(xd_ups_get_device_kind("", &address) == XD_UPS_DEVICE_NONE);
    CHECK(xd_ups_get_device_kind("nut:", nullptr) == XD_UPS_DEVICE_NONE);
    CHECK(xd_ups_get_device_kind("apcupsd:x", nullptr) == XD_UPS_DEVICE_NONE);

    /* NUT's flags, and the Status values. */
    xd_ups_reading reading;
    xd_ups_reading_init(&reading);
    char value[32];
    xd_ups_format_power_source(&reading, value, sizeof(value));
    CHECK(strcmp(value, "Unknown") == 0);
    xd_ups_format_runtime(&reading, value, sizeof(value));
    CHECK(strcmp(value, "Unknown") == 0);
    reading.is_reachable = true;
    reading.charge = 99.6;
    reading.runtime = 37 * 60 + 59;
    xd_ups_parse_nut_status("OL CHRG", &reading);
    CHECK(!reading.is_on_battery && !reading.is_low_battery && !reading.is_replace_battery);
    xd_ups_format_power_source(&reading, value, sizeof(value));
    CHECK(strcmp(value, "AC Power") == 0);
    xd_ups_format_runtime(&reading, value, sizeof(value));
    CHECK(strcmp(value, "37 minutes") == 0);
    xd_ups_format_capacity(&reading, value, sizeof(value));
    CHECK(strcmp(value, "100%") == 0);
    xd_ups_format_condition(&reading, value, sizeof(value));
    CHECK(strcmp(value, "Good") == 0);
    xd_ups_parse_nut_status("OB DISCHRG LB RB", &reading);
    CHECK(reading.is_on_battery && reading.is_low_battery && reading.is_replace_battery);
    xd_ups_format_power_source(&reading, value, sizeof(value));
    CHECK(strcmp(value, "Battery") == 0);
    xd_ups_format_condition(&reading, value, sizeof(value));
    CHECK(strcmp(value, "Replace") == 0);
    reading.runtime = 61;
    xd_ups_format_runtime(&reading, value, sizeof(value));
    CHECK(strcmp(value, "1 minute") == 0);
    /* "OBX" isn't OB. */
    xd_ups_parse_nut_status("OBX", &reading);
    CHECK(!reading.is_on_battery);
    /* UPower says nothing of the battery's condition. */
    reading.has_condition = false;
    xd_ups_format_condition(&reading, value, sizeof(value));
    CHECK(strcmp(value, "Unknown") == 0);

    /* The service: on battery at t=100, notified at 130 and every 120 s;
     * the critical alarm 10 minutes on. */
    xd_ups_watch watch = {0};
    int64_t wake = 0;
    xd_ups_parse_nut_status("OL", &reading);
    reading.is_low_battery = false;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 50, &wake) == 0 && wake == -1);
    reading.is_on_battery = true;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 100, &wake) == 0 && wake == 130);
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 129, &wake) == 0 && wake == 130);
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 130, &wake) == XD_UPS_EVENT_POWER_FAILED &&
          wake == 250);
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 250, &wake) == XD_UPS_EVENT_POWER_FAILED);
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 690, &wake) == XD_UPS_EVENT_POWER_FAILED &&
          wake == 700);
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 700, &wake) == XD_UPS_EVENT_CRITICAL &&
          wake == 810);
    /* Once per failure, even when the battery then runs low. */
    reading.is_low_battery = true;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 701, &wake) == 0);
    /* An unreachable UPS changes nothing. */
    reading.is_reachable = false;
    reading.is_on_battery = false;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 702, &wake) == 0 && watch.is_on_battery);
    reading.is_reachable = true;
    reading.is_low_battery = false;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 703, &wake) == XD_UPS_EVENT_POWER_RESTORED &&
          wake == -1);
    /* A failure shorter than the first notification's delay: nothing at
     * all, not even "restored". */
    reading.is_on_battery = true;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 800, &wake) == 0);
    reading.is_on_battery = false;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 810, &wake) == 0);
    /* Without the minutes: the critical alarm on the UPS's low battery,
     * at once; notifications off. */
    ups->critical_after = false;
    ups->notify = false;
    reading.is_on_battery = true;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 900, &wake) == 0 && wake == -1);
    reading.is_low_battery = true;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 905, &wake) == XD_UPS_EVENT_CRITICAL);
    reading.is_on_battery = reading.is_low_battery = false;
    CHECK(xd_ups_watch_step(ups, &watch, &reading, 910, &wake) == 0);
}

/* Accessibility Options' Sound tab, status indicators and reset. */
static void test_access_extra_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    const xd_access_settings *ac = &original.access;
    CHECK(ac->sticky_indicator && ac->filter_indicator && ac->mouse_keys_indicator);
    CHECK(!ac->sound_sentry && ac->sound_sentry_effect == XD_SENTRY_WINDOW && !ac->show_sounds);
    CHECK(!ac->auto_reset && ac->auto_reset_minutes == XD_AUTO_RESET_MIN);
    xd_settings_parse(XD_STR("[Accessibility]\nSoundSentry=true\nSoundSentryEffect=desktop\n"
                             "ShowSounds=true\nMouseKeysIndicator=false\nAutoReset=true\n"
                             "AutoResetMinutes=20\n"),
                      &original);
    CHECK(ac->sound_sentry && ac->sound_sentry_effect == XD_SENTRY_DESKTOP && ac->show_sounds);
    CHECK(!ac->mouse_keys_indicator && ac->auto_reset && ac->auto_reset_minutes == 20);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          parsed.access.sound_sentry_effect == XD_SENTRY_DESKTOP);
    xd_strbuf_free(&text);
    /* An unknown effect and minutes out of range: the defaults. */
    xd_settings_parse(XD_STR("[Accessibility]\nSoundSentryEffect=strobe\nAutoResetMinutes=2\n"),
                      &original);
    CHECK(ac->sound_sentry_effect == XD_SENTRY_WINDOW && ac->auto_reset_minutes == 5);
}

static void test_power_schemes(void) {
    /* No file: XP's six, with both columns. */
    GPtrArray *array = xd_power_schemes_load("/nonexistent/power-schemes.ini");
    CHECK(array->len == XD_POWER_SCHEME_COUNT);
    xd_power_scheme_def *power_scheme_def = xd_power_schemes_find(array, "Portable/Laptop");
    CHECK(power_scheme_def != nullptr && power_scheme_def->builtin == XD_POWER_PORTABLE &&
          power_scheme_def->ac.hibernate == 180 && power_scheme_def->dc.monitor == 5);
    CHECK(strcmp(xd_power_builtin_label(XD_POWER_MAX_BATTERY), "Max Battery") == 0);
    CHECK(xd_power_scheme_name_valid("My Lab") && !xd_power_scheme_name_valid("") &&
          !xd_power_scheme_name_valid("a[b]") && !xd_power_scheme_name_valid(" pad") &&
          !xd_power_scheme_name_valid("tab\there"));

    /* A changed built-in one, a deleted one and one of the user's own
     * come back as saved. */
    power_scheme_def->ac.monitor = 3;
    g_ptr_array_remove(array, xd_power_schemes_find(array, "Max Battery"));
    power_scheme_def = xd_power_schemes_put(array, "My Lab", (xd_power_timers){10, 30, 0},
                                            (xd_power_timers){2, 5, 60});
    CHECK(power_scheme_def->builtin == -1 && array->len == XD_POWER_SCHEME_COUNT);
    char *dir = g_dir_make_tmp("xd-power-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "xeondesktop", "power-schemes.ini", nullptr);
    CHECK(xd_power_schemes_save(path, array, nullptr));
    g_ptr_array_unref(array);
    array = xd_power_schemes_load(path);
    CHECK(array->len == XD_POWER_SCHEME_COUNT &&
          xd_power_schemes_find(array, "Max Battery") == nullptr);
    power_scheme_def = xd_power_schemes_find(array, "Portable/Laptop");
    CHECK(power_scheme_def != nullptr && power_scheme_def->ac.monitor == 3 &&
          power_scheme_def->ac.standby == 20);
    power_scheme_def = g_ptr_array_index(array, array->len - 1);
    CHECK(strcmp(power_scheme_def->name, "My Lab") == 0 && power_scheme_def->ac.standby == 30 &&
          power_scheme_def->dc.hibernate == 60);
    /* Saving over a name replaces its timers; a deleted built-in one can
     * be saved again. */
    xd_power_schemes_put(array, "My Lab", (xd_power_timers){1, 2, 3}, (xd_power_timers){4, 5, 6});
    power_scheme_def =
        xd_power_schemes_put(array, "Max Battery", (xd_power_timers){0}, (xd_power_timers){0});
    CHECK(power_scheme_def->builtin == XD_POWER_MAX_BATTERY &&
          array->len == XD_POWER_SCHEME_COUNT + 1);
    CHECK(xd_power_schemes_find(array, "My Lab")->dc.monitor == 4);
    g_ptr_array_unref(array);
    g_remove(path);
    char *sub = g_path_get_dirname(path);
    g_rmdir(sub);
    g_free(sub);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);
}

static void test_sound_scheme(void) {
    xd_sound_scheme original;
    xd_sound_scheme_init(&original, XD_SOUNDS_DEFAULT);
    CHECK(!xd_sound_scheme_modified(&original));
    CHECK(strcmp(original.sounds[XD_SOUND_CRITICAL_STOP], "dialog-error") == 0);
    CHECK(xd_sound_kind_of(original.sounds[XD_SOUND_LOGON]) == XD_SOUND_THEMED);
    CHECK(xd_sound_kind_of(original.sounds[XD_SOUND_MAXIMIZE]) == XD_SOUND_NONE);
    CHECK(strcmp(xd_sound_event_get(XD_SOUND_DEFAULT_BEEP)->label, "Default Beep") == 0);
    /* Paths must be absolute; theme names plain. */
    CHECK(xd_sound_scheme_set(&original, XD_SOUND_LOGON, "/usr/share/sounds/x.wav"));
    CHECK(xd_sound_kind_of(original.sounds[XD_SOUND_LOGON]) == XD_SOUND_FILE);
    CHECK(!xd_sound_scheme_set(&original, XD_SOUND_LOGON, "../x.wav"));
    CHECK(!xd_sound_scheme_set(&original, XD_SOUND_LOGON, "a b"));
    CHECK(xd_sound_scheme_set(&original, XD_SOUND_MAXIMIZE, "window-maximized"));
    CHECK(xd_sound_scheme_set(&original, XD_SOUND_ASTERISK, ""));
    CHECK(xd_sound_scheme_modified(&original));

    /* Round trip. */
    xd_strbuf text = {0};
    xd_sound_scheme_serialize(&original, &text);
    xd_sound_scheme parsed;
    xd_sound_scheme_parse(xd_strbuf_view(&text), &parsed);
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++)
        CHECK(strcmp(original.sounds[i], parsed.sounds[i]) == 0);
    xd_sound_scheme_free(&parsed);
    xd_strbuf_free(&text);
    xd_sound_scheme_free(&original);

    /* No Sounds, with one event set; bad values keep the scheme's. */
    xd_sound_scheme_parse(XD_STR("[Sounds]\nScheme=none\n[Events]\nWindowsLogon=service-login\n"
                                 "CriticalStop=relative/path.wav\nUnknown=bell\n"),
                          &original);
    CHECK(original.base == XD_SOUNDS_NONE &&
          strcmp(original.sounds[XD_SOUND_LOGON], "service-login") == 0);
    CHECK(original.sounds[XD_SOUND_CRITICAL_STOP][0] == '\0' &&
          xd_sound_scheme_modified(&original));
    xd_sound_scheme_free(&original);
    /* A broken file: Windows Default. */
    xd_sound_scheme_parse(XD_STR("[Sounds\nScheme=none\n"), &original);
    CHECK(original.base == XD_SOUNDS_DEFAULT && !xd_sound_scheme_modified(&original));
    xd_sound_scheme_free(&original);

    /* The accessibility tones: 16-bit mono WAVs, the RIFF sizes right,
     * quiet, and silent at both ends (no click). */
    for (int tone = 0; tone < XD_TONE_COUNT; tone++) {
        GBytes *wav = xd_sound_tone_wav((xd_sound_tone)tone);
        gsize len = 0;
        const uint8_t *bytes = g_bytes_get_data(wav, &len);
        CHECK(len > 44 && memcmp(bytes, "RIFF", 4) == 0 && memcmp(bytes + 8, "WAVEfmt ", 8) == 0);
        uint32_t riff = bytes[4] | (uint32_t)bytes[5] << 8 | (uint32_t)bytes[6] << 16 |
                        (uint32_t)bytes[7] << 24;
        uint32_t data = bytes[40] | (uint32_t)bytes[41] << 8 | (uint32_t)bytes[42] << 16 |
                        (uint32_t)bytes[43] << 24;
        CHECK(riff == len - 8 && data == len - 44 && bytes[22] == 1 && bytes[34] == 16);
        int peak = 0;
        for (gsize i = 44; i + 1 < len; i += 2)
            peak = MAX(peak, abs((int16_t)(bytes[i] | bytes[i + 1] << 8)));
        CHECK(peak > 1000 && peak <= INT16_MAX / 3 + 1);
        CHECK(bytes[44] == 0 && bytes[45] == 0);
        CHECK(xd_sound_tone_name((xd_sound_tone)tone)[0] != '\0');
        g_bytes_unref(wav);
    }
}

static int css_errors;

static void on_css_error([[maybe_unused]] GtkCssProvider *provider,
                         [[maybe_unused]] GtkCssSection *css_section, const GError *err,
                         [[maybe_unused]] gpointer data) {
    css_errors++;
    fprintf(stderr, "gtk css: %s\n", err->message);
}

/* The generated GTK theme must parse cleanly for every style and scheme. */
static void test_gtk_css(void) {
    /* Style properties (-GtkScrollbar-*) need the widget classes. */
    g_type_ensure(GTK_TYPE_SCROLLBAR);
    static const char *const schemes[] = {"",           "windows-classic", "high-contrast-black",
                                          "high-contrast-white", "luna-olive", "luna-silver",
                                          "luna-energyblue", "luna-dark"};
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
            xd_theme theme;
            xd_theme_init(&theme, (xd_visual_style)style, &scheme, 1.0);
            xd_strbuf css = {0};
            xd_theme_gtk_css(&theme, &css);
            CHECK(strstr(xd_strbuf_cstr(&css), "@define-color theme_selected_bg_color") != nullptr);
            GtkCssProvider *provider = gtk_css_provider_new();
            g_signal_connect(provider, "parsing-error", G_CALLBACK(on_css_error), nullptr);
            css_errors = 0;
            CHECK(gtk_css_provider_load_from_data(provider, css.data, (gssize)css.len, nullptr));
            CHECK(css_errors == 0);
            g_object_unref(provider);
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
    xd_passwd *passwd =
        xd_passwd_start(XD_SOURCE_ROOT "/tests/passwd/fake-passwd", old_pw, new_pw, nullptr);
    if (passwd == nullptr)
        return XD_PASSWD_FAILED;
    xd_passwd_result passwd_result = XD_PASSWD_RUNNING;
    for (int i = 0; i < 200 && passwd_result == XD_PASSWD_RUNNING; i++) {
        struct pollfd pfd = {.fd = xd_passwd_fd(passwd), .events = POLLIN};
        if (poll(&pfd, 1, 5000) <= 0)
            break;
        passwd_result = xd_passwd_step(passwd);
    }
    *message = g_strdup(xd_passwd_message(passwd));
    xd_passwd_free(passwd);
    return passwd_result;
}

static void test_passwd(void) {
    char *message = nullptr;
    CHECK(run_passwd("old secret", "new secret", &message) == XD_PASSWD_OK);
    g_free(message);
    CHECK(run_passwd("wrong", "new secret", &message) == XD_PASSWD_WRONG_OLD);
    CHECK(message != nullptr && strstr(message, "password unchanged") != nullptr);
    g_free(message);
    CHECK(run_passwd("old secret", "short", &message) == XD_PASSWD_REJECTED);
    CHECK(message != nullptr &&
          strcmp(message, "BAD PASSWORD: The password is shorter than 8 characters") == 0);
    g_free(message);
    /* Nothing typed may come back as a message. */
    CHECK(run_passwd("old secret", "short", &message) == XD_PASSWD_REJECTED &&
          strstr(message, "short\n") == nullptr);
    g_free(message);
}

static void check_text(char *text, const char *want) {
    if (strcmp(text, want) != 0)
        g_printerr("  got \"%s\", want \"%s\"\n", text, want);
    CHECK(strcmp(text, want) == 0);
    g_free(text);
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
    check_text(xd_intl_grouping_text("\3\377"), "3;-1"); /* CHAR_MAX where char is unsigned */

    /* Samples from the effective settings. */
    xd_intl_custom edited;
    xd_intl_custom_init(&edited);
    edited.decimal = g_strdup(",");
    edited.thousands = g_strdup(".");
    edited.grouping = g_strdup("3;3");
    edited.currency = g_strdup("€");
    edited.mon_decimal = g_strdup(",");
    edited.mon_thousands = g_strdup(".");
    edited.mon_grouping = g_strdup("3");
    edited.frac_digits = 2;
    edited.positive_format = 3;
    edited.negative_format = 8;
    check_text(xd_intl_number_sample(&edited, false), "123.456.789,00");
    check_text(xd_intl_number_sample(&edited, true), "-123.456.789,00");
    check_text(xd_intl_currency_sample(&edited, false), "123.456.789,00 €");
    check_text(xd_intl_currency_sample(&edited, true), "-123.456.789,00 €");
    edited.negative_format = 0;
    edited.frac_digits = 0;
    check_text(xd_intl_currency_sample(&edited, true), "(€123.456.789)");
    edited.time_format = g_strdup("HH.mm");
    edited.am = g_strdup("vorm.");
    edited.pm = g_strdup("nachm.");
    edited.date_format = g_strdup("dd/MM/yyyy");
    /* 2001-02-03 04:05:06 local time */
    struct tm tm = {.tm_year = 101, .tm_mon = 1, .tm_mday = 3, .tm_hour = 4, .tm_min = 5,
                    .tm_sec = 6, .tm_isdst = -1};
    time_t when = mktime(&tm);
    check_text(xd_intl_time_sample("C.UTF-8", &edited, when), "04.05");
    g_free(edited.time_format);
    edited.time_format = g_strdup("h:mm tt");
    check_text(xd_intl_time_sample("C.UTF-8", &edited, when), "4:05 vorm.");
    check_text(xd_intl_date_sample("C.UTF-8", &edited, when, false), "03/02/2001");

    /* formats.ini keeps only what was changed. */
    char *dir = g_dir_make_tmp("xd-custom-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "formats.ini", nullptr);
    xd_intl_custom blank;
    xd_intl_custom_init(&blank);
    CHECK(xd_intl_custom_empty(&blank));
    CHECK(xd_intl_custom_save(path, "C.UTF-8", &blank, nullptr) &&
          !g_file_test(path, G_FILE_TEST_EXISTS));
    blank.thousands = g_strdup(" ");
    blank.grouping = g_strdup("3;2");
    blank.negative_format = 12;
    blank.time_format = g_strdup("HH:mm");
    CHECK(!xd_intl_custom_empty(&blank));
    CHECK(xd_intl_custom_save(path, "C.UTF-8", &blank, nullptr));
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
        char *source = xd_intl_custom_source("en_US.UTF-8", &blank);
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
    xd_intl_custom_clear(&blank);
    xd_intl_custom_clear(&edited);
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

    char *first = xd_intl_first("us,de");
    CHECK(strcmp(first, "us") == 0);
    g_free(first);
    first = xd_intl_first(nullptr);
    CHECK(strcmp(first, "") == 0);
    g_free(first);
    first = xd_intl_replace_first("us,de", "fr");
    CHECK(strcmp(first, "fr,de") == 0);
    g_free(first);
    first = xd_intl_replace_first(",nodeadkeys", "dvorak");
    CHECK(strcmp(first, "dvorak,nodeadkeys") == 0);
    g_free(first);
    first = xd_intl_replace_first("", "fr");
    CHECK(strcmp(first, "fr") == 0);
    g_free(first);

    /* The formats file round trip, and removing it. */
    char *dir = g_dir_make_tmp("xd-intl-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "environment.d", "60-xeondesktop-locale.conf", nullptr);
    CHECK(xd_intl_read_formats(path) == nullptr);
    CHECK(xd_intl_write_formats(path, "C.UTF-8", nullptr));
    first = xd_intl_read_formats(path);
    CHECK(first != nullptr && strcmp(first, "C.UTF-8") == 0);
    g_free(first);
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

    first = xd_intl_display_name("C.UTF-8");
    CHECK(strcmp(first, "C.UTF-8") == 0);
    g_free(first);
    first = xd_intl_display_name("xx_XX.UTF-8");
    CHECK(strcmp(first, "xx_XX.UTF-8") == 0);
    g_free(first);

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

/* The note under automatic logon names the display manager and says
 * whether it follows AccountsService's setting. */
static void test_autologin_note(void) {
    char *note = xd_accounts_autologin_note("gdm.service");
    CHECK(strstr(note, "GDM") != nullptr && strstr(note, "follows this setting") != nullptr);
    g_free(note);
    note = xd_accounts_autologin_note("gdm3.service"); /* Debian's */
    CHECK(strstr(note, "GDM") != nullptr && strstr(note, "follows") != nullptr);
    g_free(note);
    note = xd_accounts_autologin_note("sddm.service");
    CHECK(strstr(note, "SDDM") != nullptr && strstr(note, "no effect") != nullptr &&
          strstr(note, "GDM") == nullptr);
    g_free(note);
    note = xd_accounts_autologin_note("lightdm.service");
    CHECK(strstr(note, "LightDM, which keeps its own") != nullptr);
    g_free(note);
    /* Unknown: named, with what's known in general. */
    note = xd_accounts_autologin_note("mydm.service");
    CHECK(strstr(note, "is mydm.") != nullptr && strstr(note, "as GDM does") != nullptr);
    g_free(note);
    char *none = xd_accounts_autologin_note(nullptr);
    char *empty = xd_accounts_autologin_note("");
    CHECK(strcmp(none, empty) == 0 && strstr(none, "SDDM and LightDM keep their own") != nullptr);
    g_free(none);
    g_free(empty);
    CHECK(xd_accounts_display_manager_unit(nullptr) == nullptr);
}

static void test_users(void) {
    CHECK(xd_users_valid_name("bob") && xd_users_valid_name("a1_b-c"));
    CHECK(!xd_users_valid_name("") && !xd_users_valid_name("Bob") && !xd_users_valid_name("1bob"));
    CHECK(!xd_users_valid_name("-bob") && !xd_users_valid_name("_bob") &&
          !xd_users_valid_name("bo b") && !xd_users_valid_name(nullptr));
    CHECK(xd_users_valid_name("abcdefghijklmnopqrstuvwxyzabcdef") &&
          !xd_users_valid_name("abcdefghijklmnopqrstuvwxyzabcdefg"));

    char *name = xd_users_suggest_name("Robert Smith");
    CHECK(strcmp(name, "robertsmith") == 0);
    g_free(name);
    name = xd_users_suggest_name("2nd O'Brien 3");
    CHECK(strcmp(name, "ndobrien3") == 0 && xd_users_valid_name(name));
    g_free(name);
    name = xd_users_suggest_name("\u4e2d\u6587");
    CHECK(name != nullptr && !xd_users_valid_name(name)); /* nothing usable: "" or "??" dropped */
    g_free(name);

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
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(!original.folders.new_window && !original.folders.single_click &&
          !original.folders.show_hidden);
    CHECK(original.folders.full_path_address && !original.folders.full_path_title &&
          original.folders.remember_views);
    /* Tabs instead of windows is off unless chosen: XP's behaviour. */
    CHECK(!original.folders.new_tabs);
    /* Show common tasks in folders: XP's default. */
    CHECK(original.folders.common_tasks);
    xd_settings_parse(XD_STR("[Folders]\nCommonTasks=false\n"), &original);
    CHECK(!original.folders.common_tasks && original.folders.remember_views);
    xd_settings_parse(XD_STR("[Folders]\nNewWindow=true\nNewTabs=true\nSingleClick=1\n"
                             "ShowHidden=true\nHideExtensions=true\nFullPathAddress=false\n"
                             "FullPathTitle=true\nRememberViews=false\nDefaultView=Details\n"),
                      &original);
    CHECK(original.folders.new_window && original.folders.new_tabs &&
          original.folders.single_click && original.folders.show_hidden);
    CHECK(original.folders.hide_extensions && !original.folders.full_path_address &&
          original.folders.full_path_title);
    CHECK(!original.folders.remember_views &&
          strcmp(original.folders.default_view, "details") == 0);
    CHECK(original.folders.common_tasks); /* not in the text: the default */
    original.folders.common_tasks = false;
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          strcmp(parsed.folders.default_view, "details") == 0);
    CHECK(!parsed.folders.common_tasks); /* saved as CommonTasks=false */
    xd_strbuf_free(&text);
    /* An unknown view keeps the default. */
    xd_settings_parse(XD_STR("[Folders]\nDefaultView=../x\n"), &original);
    CHECK(original.folders.default_view[0] == '\0');

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
    const xd_mime_glob *mime_glob = g_ptr_array_index(pkg.globs, 0);
    CHECK(strcmp(mime_glob->pattern, "*.TST") == 0 && mime_glob->weight == 60 &&
          mime_glob->case_sensitive);
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

    char *name = xd_font_display_name("DejaVu Sans Bold", "DejaVu Sans", "Bold", "TrueType");
    CHECK(strcmp(name, "DejaVu Sans Bold (TrueType)") == 0);
    g_free(name);
    name = xd_font_display_name(nullptr, "Foo", "Regular", "OpenType");
    CHECK(strcmp(name, "Foo (OpenType)") == 0);
    g_free(name);
    name = xd_font_display_name("", "Foo", "Italic", nullptr);
    CHECK(strcmp(name, "Foo Italic") == 0);
    g_free(name);

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

    char *version = xd_font_version_text(0x00025eb8); /* 2.37 */
    CHECK(strcmp(version, "Version 2.37") == 0);
    g_free(version);
    version = xd_font_version_text(0x0001ffff); /* rounds up to 2.00 */
    CHECK(strcmp(version, "Version 2.00") == 0);
    g_free(version);
    version = xd_font_version_text(0);
    CHECK(version[0] == '\0');
    g_free(version);
}

/* [Mouse], [Accessibility] and the [Keyboard] keys the applets add. */
static void test_input_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(!original.mouse.left_handed && original.mouse.acceleration &&
          original.mouse.double_click_ms == 500);
    CHECK(original.mouse.wheel_lines == 3 && original.mouse.cursor_size == 24 &&
          original.mouse.cursor_theme[0] == 0);
    CHECK(original.keyboard.cursor_blink_ms == 530 && !original.access.sticky_keys &&
          original.access.sticky_lock);

    xd_settings_parse(XD_STR("[Mouse]\nLeftHanded=true\nSpeed=-4\nAcceleration=false\n"
                             "DoubleClickMs=350\nWheelLines=6\nCursorTheme=Breeze_Light\n"
                             "CursorSize=48\n[Keyboard]\nCursorBlinkMs=0\n"
                             "[Accessibility]\nStickyKeys=true\nStickyKeysLock=false\n"
                             "BounceKeys=true\nBounceMs=700\n"),
                      &original);
    CHECK(original.mouse.left_handed && original.mouse.speed == -4 && !original.mouse.acceleration);
    CHECK(original.mouse.double_click_ms == 350 && original.mouse.wheel_lines == 6 &&
          original.mouse.cursor_size == 48);
    CHECK(strcmp(original.mouse.cursor_theme, "Breeze_Light") == 0 &&
          original.keyboard.cursor_blink_ms == 0);
    CHECK(original.access.sticky_keys && !original.access.sticky_lock &&
          original.access.filter_keys && original.access.bounce_ms == 700);

    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) &&
          memcmp(&original.mouse, &parsed.mouse, sizeof(original.mouse)) == 0);
    CHECK(memcmp(&original.access, &parsed.access, sizeof(original.access)) == 0);
    xd_strbuf_free(&text);

    /* Out of range keeps the default; a theme can't be a path. */
    xd_settings_parse(XD_STR("[Mouse]\nSpeed=11\nDoubleClickMs=50\nWheelLines=0\n"
                             "CursorTheme=../x\nCursorSize=4\n[Accessibility]\nBounceMs=5\n"),
                      &original);
    CHECK(original.mouse.speed == 0 && original.mouse.double_click_ms == 500 &&
          original.mouse.wheel_lines == 3);
    CHECK(original.mouse.cursor_theme[0] == 0 && original.mouse.cursor_size == 24 &&
          original.access.bounce_ms == 500);
    xd_settings_parse(XD_STR("[Mouse]\nCursorTheme=.hidden\n"), &original);
    CHECK(original.mouse.cursor_theme[0] == 0);

    /* [Touchpad]: tapping, two-finger scrolling, typing guard and the
     * desktop's swipes on, as Windows' precision touchpads come. */
    xd_settings_defaults(&original);
    const xd_touchpad_settings *touchpad = &original.mouse.touchpad;
    CHECK(touchpad->tap && touchpad->tap_drag && !touchpad->drag_lock);
    CHECK(touchpad->scroll == XD_TOUCHPAD_SCROLL_TWO_FINGER && !touchpad->natural_scroll &&
          touchpad->scroll_speed == 100);
    CHECK(touchpad->click == XD_TOUCHPAD_CLICK_DEFAULT && touchpad->disable_typing &&
          !touchpad->disable_with_mouse);
    CHECK(touchpad->speed == 0 && touchpad->acceleration && touchpad->gestures);
    xd_settings_parse(XD_STR("[Touchpad]\nTapToClick=false\nTapAndDrag=false\nDragLock=true\n"
                             "Scrolling=edge\nNaturalScrolling=true\nScrollSpeed=150\n"
                             "ClickMethod=fingers\nDisableWhileTyping=false\n"
                             "DisableWithMouse=true\nSpeed=3\nAcceleration=false\n"
                             "Gestures=false\n[Mouse]\nSpeed=-2\n"),
                      &original);
    CHECK(!touchpad->tap && !touchpad->tap_drag && touchpad->drag_lock);
    CHECK(touchpad->scroll == XD_TOUCHPAD_SCROLL_EDGE && touchpad->natural_scroll &&
          touchpad->scroll_speed == 150);
    CHECK(touchpad->click == XD_TOUCHPAD_CLICK_FINGERS && !touchpad->disable_typing &&
          touchpad->disable_with_mouse);
    CHECK(touchpad->speed == 3 && !touchpad->acceleration && !touchpad->gestures);
    CHECK(original.mouse.speed == -2); /* the mouse's own, apart */
    xd_settings_serialize(&original, &text);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(memcmp(&original.mouse.touchpad, &parsed.mouse.touchpad, sizeof(*touchpad)) == 0);
    xd_strbuf_free(&text);
    /* Out of range, or a word it doesn't know, keeps the default. */
    xd_settings_parse(XD_STR("[Touchpad]\nScrollSpeed=5000\nScrolling=diagonal\n"
                             "ClickMethod=sideways\nSpeed=-30\n"),
                      &original);
    CHECK(touchpad->scroll_speed == 100 && touchpad->scroll == XD_TOUCHPAD_SCROLL_TWO_FINGER);
    CHECK(touchpad->click == XD_TOUCHPAD_CLICK_DEFAULT && touchpad->speed == 0);

    /* XP's defaults: off, but every shortcut on, with its warning. */
    xd_settings_defaults(&original);
    CHECK(!original.access.filter_keys && !original.access.toggle_keys &&
          !original.access.mouse_keys);
    CHECK(original.access.sticky_shortcut && original.access.filter_shortcut &&
          original.access.toggle_shortcut);
    CHECK(original.access.mouse_keys_shortcut && original.access.warn &&
          original.access.sound_on_off);
    CHECK(original.access.slow_keys && original.access.slow_ms == 1000 &&
          !original.mouse.click_lock);
    CHECK(original.mouse.click_lock_ms == 1200 && original.access.mouse_keys_numlock);
    xd_settings_parse(XD_STR("[Mouse]\nClickLock=true\nClickLockMs=800\n[Accessibility]\n"
                             "FilterKeys=true\nSlowKeys=false\nSlowKeysMs=300\n"
                             "FilterKeysNoRepeat=true\nToggleKeys=true\nToggleKeysShortcut=0\n"
                             "MouseKeys=true\nMouseKeysSpeed=200\nMouseKeysAccelMs=1500\n"
                             "MouseKeysNumLock=false\nWarnOnShortcut=false\n"
                             "StickyKeysTwoKeysOff=false\n"),
                      &original);
    CHECK(original.mouse.click_lock && original.mouse.click_lock_ms == 800);
    CHECK(original.access.filter_keys && !original.access.slow_keys &&
          original.access.slow_ms == 300);
    CHECK(original.access.filter_no_repeat && original.access.toggle_keys &&
          !original.access.toggle_shortcut);
    CHECK(original.access.mouse_keys && original.access.mouse_keys_speed == 200);
    CHECK(original.access.mouse_keys_accel_ms == 1500 && !original.access.mouse_keys_numlock);
    CHECK(!original.access.warn && !original.access.sticky_two_keys_off &&
          original.access.sticky_sound);
    xd_settings_serialize(&original, &text);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    xd_strbuf_free(&text);
    CHECK(memcmp(&original.access, &parsed.access, sizeof(original.access)) == 0);
    CHECK(memcmp(&original.mouse, &parsed.mouse, sizeof(original.mouse)) == 0);
    /* Out of range keeps the default. */
    xd_settings_parse(XD_STR("[Mouse]\nClickLockMs=50\n[Accessibility]\nMouseKeysSpeed=9000\n"),
                      &original);
    CHECK(original.mouse.click_lock_ms == 1200 && original.access.mouse_keys_speed == 80);

    /* High Contrast switches to Classic and back to what was there. */
    xd_settings_defaults(&original);
    original.style = XD_STYLE_LUNA;
    g_strlcpy(original.scheme, "windows-standard", sizeof(original.scheme));
    xd_settings_set_high_contrast(&original, true, "high-contrast-black");
    CHECK(original.access.high_contrast && original.style == XD_STYLE_CLASSIC);
    CHECK(strcmp(original.scheme, "high-contrast-black") == 0);
    /* Another high-contrast scheme keeps the look to go back to. */
    xd_settings_set_high_contrast(&original, true, "high-contrast-white");
    CHECK(strcmp(original.scheme, "high-contrast-white") == 0 &&
          original.access.saved_style == XD_STYLE_LUNA);
    xd_settings_serialize(&original, &text);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    xd_strbuf_free(&text);
    CHECK(parsed.access.high_contrast &&
          strcmp(parsed.access.saved_scheme, "windows-standard") == 0);
    xd_settings_set_high_contrast(&parsed, false, "");
    CHECK(!parsed.access.high_contrast && parsed.style == XD_STYLE_LUNA);
    CHECK(strcmp(parsed.scheme, "windows-standard") == 0);
    /* Off when it's already off changes nothing. */
    xd_settings_set_high_contrast(&parsed, false, "");
    CHECK(parsed.style == XD_STYLE_LUNA && strcmp(parsed.scheme, "windows-standard") == 0);
}

static void test_kbd_layouts(void) {
    xd_kbd_layout kbd_layout[XD_KBD_LAYOUTS_MAX];
    CHECK(xd_kbd_layouts_parse("", "", kbd_layout) == 0);
    CHECK(xd_kbd_layouts_parse("us,de", ",nodeadkeys", kbd_layout) == 2);
    CHECK(strcmp(kbd_layout[0].name, "us") == 0 && kbd_layout[0].variant[0] == 0);
    CHECK(strcmp(kbd_layout[1].name, "de") == 0 &&
          strcmp(kbd_layout[1].variant, "nodeadkeys") == 0);
    /* Fewer variants than layouts; at most four layouts. */
    CHECK(xd_kbd_layouts_parse("us, fr ,ru,de,gb", "dvorak", kbd_layout) == 4);
    CHECK(strcmp(kbd_layout[0].variant, "dvorak") == 0 && strcmp(kbd_layout[1].name, "fr") == 0 &&
          kbd_layout[1].variant[0] == 0 && strcmp(kbd_layout[3].name, "de") == 0);

    char layouts[64], variants[64];
    CHECK(
        xd_kbd_layouts_format(kbd_layout, 2, layouts, sizeof(layouts), variants, sizeof(variants)));
    CHECK(strcmp(layouts, "us,fr") == 0 && strcmp(variants, "dvorak,") == 0);
    xd_kbd_layouts_parse("us,de", "", kbd_layout);
    CHECK(
        xd_kbd_layouts_format(kbd_layout, 2, layouts, sizeof(layouts), variants, sizeof(variants)));
    CHECK(strcmp(layouts, "us,de") == 0 && variants[0] == 0);
    CHECK(!xd_kbd_layouts_format(kbd_layout, 2, layouts, 4, variants, sizeof(variants)) &&
          layouts[0] == 0);

    char *option = xd_kbd_option_in_group("caps:ctrl_modifier,grp:alt_shift_toggle", "grp");
    CHECK(option != nullptr && strcmp(option, "grp:alt_shift_toggle") == 0);
    g_free(option);
    /* "grp_led:" is another group. */
    CHECK(xd_kbd_option_in_group("grp_led:scroll", "grp") == nullptr);
    option = xd_kbd_option_set("caps:ctrl_modifier", "compose:ralt", true);
    CHECK(strcmp(option, "caps:ctrl_modifier,compose:ralt") == 0);
    char *o2 = xd_kbd_option_set(option, "compose:ralt", true); /* never twice */
    CHECK(strcmp(o2, "caps:ctrl_modifier,compose:ralt") == 0);
    g_free(option);
    option = xd_kbd_option_set(o2, "caps:ctrl_modifier", false);
    CHECK(strcmp(option, "compose:ralt") == 0);
    g_free(o2);
    g_free(option);
    option = xd_kbd_options_clear_group("grp:win_space_toggle,grp_led:scroll,,grp:alt_caps_toggle",
                                        "grp");
    CHECK(strcmp(option, "grp_led:scroll") == 0);
    g_free(option);
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
    xd_settings original;
    xd_settings_parse(XD_STR("[Appearance]\nStyle=luna\nQtStyle=kvantum-dark\n"), &original);
    CHECK(strcmp(original.qt_style, "kvantum-dark") == 0);
    CHECK(strcmp(xd_settings_qt_style(&original), "kvantum-dark") == 0);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    CHECK(strstr(xd_strbuf_cstr(&text), "QtStyle=kvantum-dark") != nullptr);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(xd_settings_equal(&original, &parsed) && strcmp(parsed.qt_style, "kvantum-dark") == 0);
    xd_strbuf_free(&text);
    /* It goes into the environment: no spaces, paths or shell syntax. */
    static const char *const bad[] = {
        "[Appearance]\nQtStyle=../breeze\n", "[Appearance]\nQtStyle=Br eeze\n",
        "[Appearance]\nQtStyle=a;b\n",
        "[Appearance]\nQtStyle=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"};
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        xd_settings_parse(xd_str_from_cstr(bad[i]), &original);
        CHECK(original.qt_style[0] == '\0');
    }
    /* Automatic: 2000's bevels for Classic, Fusion for the rest. */
    xd_settings_defaults(&original);
    CHECK(original.qt_style[0] == '\0');
    CHECK(strcmp(xd_settings_qt_style(&original), "Windows") == 0);
    static const xd_visual_style fusion[] = {XD_STYLE_LUNA, XD_STYLE_WHISTLER, XD_STYLE_AERO};
    for (size_t i = 0; i < G_N_ELEMENTS(fusion); i++) {
        original.style = fusion[i];
        CHECK(strcmp(xd_settings_qt_style(&original), "Fusion") == 0);
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
    xd_settings_defaults(&original);
    CHECK(xd_settings_qt_env(&original));
    CHECK(g_strcmp0(g_getenv("QT_QPA_PLATFORMTHEME"), "gtk3") == 0);
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "Windows") == 0);
    CHECK(xd_qt_env_ours("QT_QPA_PLATFORMTHEME") && xd_qt_env_ours("QT_STYLE_OVERRIDE"));
    /* Our palette with our GTK theme, none with another. */
    CHECK(g_str_has_suffix(g_getenv("QT_GUI_GTK_JSON"), "/XeonDesktop/" XD_QT_PALETTE_FILE));
    CHECK(!xd_settings_qt_env(&original)); /* nothing new */
    snprintf(original.gtk_theme, sizeof(original.gtk_theme), "%s", "Adwaita");
    CHECK(xd_settings_qt_env(&original));
    CHECK(g_strcmp0(g_getenv("QT_GUI_GTK_JSON"), "") == 0 && xd_qt_env_ours("QT_GUI_GTK_JSON"));
    original.gtk_theme[0] = '\0';
    original.style = XD_STYLE_AERO; /* a child of ours, after a change */
    CHECK(xd_settings_qt_env(&original));
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "Fusion") == 0);
    /* The user's own (qt6ct, Kvantum), set before the desktop started... */
    for (size_t i = 0; i < G_N_ELEMENTS(vars); i++)
        g_unsetenv(vars[i]);
    g_setenv("QT_QPA_PLATFORMTHEME", "qt6ct", TRUE);
    g_setenv("QT_STYLE_OVERRIDE", "kvantum", TRUE);
    xd_settings_qt_env(&original); /* only adds our palette file, which they ignore */
    CHECK(!xd_settings_qt_env(&original));
    CHECK(g_strcmp0(g_getenv("QT_QPA_PLATFORMTHEME"), "qt6ct") == 0);
    CHECK(g_strcmp0(g_getenv("QT_STYLE_OVERRIDE"), "kvantum") == 0);
    CHECK(!xd_qt_env_ours("QT_STYLE_OVERRIDE"));
    /* ...or over ours, for one program started from a terminal. */
    g_unsetenv("QT_STYLE_OVERRIDE");
    CHECK(xd_settings_qt_env(&original) && xd_qt_env_ours("QT_STYLE_OVERRIDE"));
    g_setenv("QT_STYLE_OVERRIDE", "Breeze", TRUE);
    CHECK(!xd_settings_qt_env(&original));
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
    for (char **dir = found; *dir != nullptr; dir++) {
        CHECK(g_file_test(*dir, G_FILE_TEST_IS_DIR));
        for (char **later_dir = dir + 1; *later_dir != nullptr; later_dir++)
            CHECK(strcmp(*dir, *later_dir) != 0);
    }
    g_strfreev(found);
}

/* The colour Qt's palette file gives `role` in its active group. */
static char *qt_palette_color(JsonNode *root, const char *palette, const char *role) {
    JsonObject *palettes =
        json_object_get_object_member(json_node_get_object(root), "QtGtk3Palettes");
    palettes = palettes != nullptr ? json_object_get_object_member(palettes, palette) : nullptr;
    JsonArray *entries =
        palettes != nullptr ? json_object_get_array_member(palettes, role) : nullptr;
    for (guint i = 0; entries != nullptr && i < json_array_get_length(entries); i++) {
        JsonObject *entry_object = json_array_get_object_element(entries, i);
        if (strcmp(json_object_get_string_member(entry_object, "ColorGroup"), "Active") != 0 ||
            strcmp(json_object_get_string_member(entry_object, "SourceType"), "Fixed") != 0)
            continue;
        JsonObject *data = json_object_get_object_member(entry_object, "SourceData");
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
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    xd_strbuf json = {0};
    xd_qt_palette_json(&theme, &json);
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
        char *got_color = qt_palette_color(root, want[i].palette, want[i].role);
        CHECK(g_strcmp0(got_color, want[i].color) == 0);
        g_free(got_color);
    }
    g_object_unref(parser);
    /* Luna: XP's white menus and its selection blue. */
    xd_theme_init(&theme, XD_STYLE_LUNA, &scheme, 1.0);
    xd_qt_palette_json(&theme, &json);
    parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, xd_strbuf_cstr(&json), (gssize)json.len, nullptr));
    root = json_parser_get_root(parser);
    char *got_color = root != nullptr ? qt_palette_color(root, "MenuPalette", "Window") : nullptr;
    CHECK(g_strcmp0(got_color, "#ffffff") == 0);
    g_free(got_color);
    got_color = root != nullptr ? qt_palette_color(root, "SystemPalette", "Highlight") : nullptr;
    CHECK(g_strcmp0(got_color, "#316ac5") == 0);
    g_free(got_color);
    g_object_unref(parser);
    xd_strbuf_free(&json);
}

static void test_cursor_themes(void) {
    char *root = g_dir_make_tmp("xd-cursors-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    char *first_root = g_build_filename(root, "a", nullptr),
         *second_root = g_build_filename(root, "b", nullptr);
    /* a: "Zed" (with cursors) and "plain" (an icon theme, no cursors);
     * b: another "Zed" (hidden by a's), "Alpha", "default". */
    static const char *const made[] = {"a/Zed/cursors", "a/plain/48x48", "b/Zed/cursors",
                                       "b/Alpha/cursors", "b/default/cursors"};
    for (size_t i = 0; i < G_N_ELEMENTS(made); i++) {
        char *made_dir = g_build_filename(root, made[i], nullptr);
        g_mkdir_with_parents(made_dir, 0700);
        g_free(made_dir);
    }
    char *index = g_build_filename(first_root, "Zed", "index.theme", nullptr);
    CHECK(g_file_set_contents(index, "[Icon Theme]\nName=Zed Cursors\n", -1, nullptr));
    const char *dirs[] = {first_root, second_root, "/nonexistent", nullptr};
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
    g_free(first_root);
    g_free(second_root);
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
static void check_calendar(const xd_schedule *schedule, const char *want) {
    char *cal = xd_schedule_calendar(schedule);
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

static void check_describe(const xd_schedule *schedule, const char *want) {
    char *text = xd_schedule_describe(schedule);
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
        xd_schedule schedule;
        bool ok = xd_schedule_from_calendar(foreign[i], &schedule);
        CHECK(!ok && schedule.kind == XD_SCHED_CUSTOM);
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
    xd_task task = {.name = "Back up 100%",
                    .argv = run,
                    .workdir = "/home/u",
                    .sched = daily,
                    .max_run_sec = 72 * 3600};
    check_str(xd_task_service_text(&task),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Back up 100%%\n"
              "\n"
              "[Service]\n"
              "Type=exec\n"
              "ExecStart=\"/usr/bin/backup\" \"--to\" \"/mnt/My Disk\"\n"
              "WorkingDirectory=/home/u\n"
              "RuntimeMaxSec=259200\n");
    check_str(xd_task_timer_text(&task),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Back up 100%%\n"
              "\n"
              "[Timer]\n"
              "OnCalendar=*-*-* 09:00:00\n"
              "\n"
              "[Install]\n"
              "WantedBy=timers.target\n");
    task = (xd_task){.name = " Hello ", .argv = run, .sched = startup};
    check_str(xd_task_service_text(&task),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Hello\n"
              "\n"
              "[Service]\n"
              "Type=exec\n"
              "ExecStart=\"/usr/bin/backup\" \"--to\" \"/mnt/My Disk\"\n");
    check_str(xd_task_timer_text(&task),
              "# A scheduled task, written by Scheduled Tasks (xd-schedtasks).\n"
              "[Unit]\n"
              "Description=Hello\n"
              "\n"
              "[Timer]\n"
              "OnStartupSec=0\n"
              "\n"
              "[Install]\n"
              "WantedBy=timers.target\n");
    task.sched = logon;
    check_str(xd_task_timer_text(&task),
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
    task.name = "a/b";
    CHECK(xd_task_service_text(&task) == nullptr && xd_task_timer_text(&task) == nullptr);
    task.name = "ok";
    task.workdir = "relative";
    CHECK(xd_task_service_text(&task) == nullptr);
    task.workdir = nullptr;
    task.sched = weekly; /* no days */
    CHECK(xd_task_timer_text(&task) == nullptr);
    task.sched.kind = XD_SCHED_CUSTOM;
    CHECK(xd_task_timer_text(&task) == nullptr);
    task.sched = daily;
    task.argv = empty;
    CHECK(xd_task_service_text(&task) == nullptr);
}

static bool type_is(xd_bt_type bt_type, const char *icon, const char *name) {
    return g_strcmp0(bt_type.icon, icon) == 0 && g_strcmp0(bt_type.name, name) == 0;
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

static bool close_to(double left, double right) {
    return left - right < 1e-3 && right - left < 1e-3;
}

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
    size_t count = xd_joy_button_order(codes, XD_ARRAY_LEN(codes), out);
    CHECK(count == 4);
    CHECK(out[0] == BTN_SOUTH && out[1] == BTN_EAST && out[2] == BTN_TRIGGER_HAPPY1 &&
          out[3] == BTN_0);
    CHECK(xd_joy_button_order(codes, 0, out) == 0);
}

static void test_bluetooth_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(original.bluetooth_icon);
    xd_settings_parse(XD_STR("[Bluetooth]\nShowIcon=false\n"), &original);
    CHECK(!original.bluetooth_icon);
    xd_strbuf out = {0};
    xd_settings_serialize(&original, &out);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&out), &parsed);
    CHECK(!parsed.bluetooth_icon);
    xd_strbuf_free(&out);
}

/* [NightLight] and [ColorProfiles]: parsing, bad values, round trip, and
 * two programs' changes merging. */
static void test_nightlight_settings(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(!original.night_light.enabled);
    CHECK(original.night_light.schedule == XD_NIGHTLIGHT_SUNSET);
    CHECK(!original.night_light.has_location);
    xd_settings_parse(
        XD_STR("[NightLight]\nEnabled=true\nSchedule=manual\nTemperature=2500\n"
               "TurnOn=20:30\nTurnOff=06:15\nLatitude=51.5074\nLongitude=-0.1278\n"
               "AutomaticLocation=false\n"
               "[ColorProfiles]\nDell Inc. U2415 7MT0=/home/u/.local/share/icc/d.icc\n"
               "HEADLESS-1=relative.icc\n"),
        &original);
    const xd_nightlight_settings *nightlight_settings = &original.night_light;
    CHECK(nightlight_settings->enabled && nightlight_settings->schedule == XD_NIGHTLIGHT_MANUAL &&
          nightlight_settings->temperature == 2500);
    CHECK(nightlight_settings->on_minute == 20 * 60 + 30 &&
          nightlight_settings->off_minute == 6 * 60 + 15);
    CHECK(nightlight_settings->has_location &&
          fabs(nightlight_settings->latitude - 51.5074) < 1e-9 &&
          fabs(nightlight_settings->longitude + 0.1278) < 1e-9);
    CHECK(!nightlight_settings->auto_location);
    CHECK(original.color_profile_count == 1);
    CHECK(g_strcmp0(xd_settings_color_profile(&original, "Dell Inc. U2415 7MT0"),
                    "/home/u/.local/share/icc/d.icc") == 0);
    CHECK(xd_settings_color_profile(&original, "HEADLESS-1") == nullptr);

    /* Round trip, and a locale with a decimal comma doesn't matter. */
    xd_strbuf out = {0};
    xd_settings_serialize(&original, &out);
    CHECK(strstr(xd_strbuf_cstr(&out), "Latitude=51.5074") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&out), "TurnOff=06:15") != nullptr);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&out), &parsed);
    CHECK(xd_settings_equal(&original, &parsed));
    xd_strbuf_free(&out);

    /* Out of range or malformed: the value before stays. */
    xd_settings_parse(XD_STR("[NightLight]\nTemperature=99999\nTurnOn=25:00\nTurnOff=7\n"
                             "Latitude=91\nLongitude=abc\nSchedule=sometimes\n"),
                      &parsed);
    CHECK(parsed.night_light.temperature == 3400 && parsed.night_light.on_minute == 21 * 60 &&
          parsed.night_light.off_minute == 7 * 60 && !parsed.night_light.has_location &&
          parsed.night_light.schedule == XD_NIGHTLIGHT_SUNSET);

    /* Keys GKeyFile can't hold, relative paths, and removal. */
    CHECK(!xd_settings_set_color_profile(&parsed, "A=B", "/x.icc"));
    CHECK(!xd_settings_set_color_profile(&parsed, "[A]", "/x.icc"));
    CHECK(!xd_settings_set_color_profile(&parsed, "A", "x.icc"));
    CHECK(xd_settings_set_color_profile(&parsed, "A", "/x.icc"));
    CHECK(xd_settings_set_color_profile(&parsed, "A", "/y.icc"));
    CHECK(parsed.color_profile_count == 1 && strcmp(parsed.color_profiles[0].path, "/y.icc") == 0);
    CHECK(xd_settings_set_color_profile(&parsed, "A", nullptr) && parsed.color_profile_count == 0);

    /* Color Management sets a profile while Display Properties turns night
     * light on: both survive. */
    xd_settings base, mine, now;
    xd_settings_defaults(&base);
    mine = base;
    now = base;
    CHECK(xd_settings_set_color_profile(&mine, "HEADLESS-1", "/p.icc"));
    now.night_light.enabled = true;
    xd_settings_merge(&base, &mine, &now);
    CHECK(now.night_light.enabled);
    CHECK(g_strcmp0(xd_settings_color_profile(&now, "HEADLESS-1"), "/p.icc") == 0);
    /* ...and removing it again goes through a merge too. */
    base = mine;
    xd_settings_set_color_profile(&mine, "HEADLESS-1", "");
    xd_settings_merge(&base, &mine, &now);
    CHECK(xd_settings_color_profile(&now, "HEADLESS-1") == nullptr && now.night_light.enabled);
}

/* Night light's arithmetic: colour temperatures, the sun, the schedule's
 * transitions, and the gamma ramps. */
static void test_nightlight_math(void) {
    double rgb[3];
    xd_blackbody_rgb(XD_NIGHTLIGHT_DAY, rgb);
    CHECK(rgb[0] == 1.0 && rgb[1] == 1.0 && rgb[2] == 1.0);
    xd_blackbody_rgb(9000, rgb); /* never bluer than day */
    CHECK(rgb[0] == 1.0 && rgb[1] == 1.0 && rgb[2] == 1.0);
    xd_blackbody_rgb(3400, rgb);
    CHECK(rgb[0] == 1.0 && fabs(rgb[1] - 0.746) < 0.005 && fabs(rgb[2] - 0.541) < 0.005);
    xd_blackbody_rgb(1700, rgb);
    CHECK(rgb[0] == 1.0 && fabs(rgb[1] - 0.475) < 0.005 && rgb[2] == 0.0);
    /* Warmer is redder all the way down. */
    double prev[3] = {1, 1, 1};
    bool monotonic = true;
    for (int k = XD_NIGHTLIGHT_K_DAY; k >= XD_NIGHTLIGHT_K_MIN; k -= 100) {
        xd_blackbody_rgb(k, rgb);
        monotonic = monotonic && rgb[0] == 1.0 && rgb[1] <= prev[1] && rgb[2] <= prev[2];
        memcpy(prev, rgb, sizeof(prev));
    }
    CHECK(monotonic);

    /* Sunrise and sunset in UTC minutes, against published tables (to the
     * minute; NOAA's approximation is good to a couple). */
    double rise, set;
    CHECK(xd_sun_times(2026, 6, 21, 51.5074, -0.1278, &rise, &set) == XD_SUN_RISES_AND_SETS);
    CHECK(fabs(rise - (3 * 60 + 43)) < 3 && fabs(set - (20 * 60 + 21)) < 3); /* London */
    CHECK(xd_sun_times(2026, 12, 21, 40.7128, -74.0060, &rise, &set) == XD_SUN_RISES_AND_SETS);
    CHECK(fabs(rise - (12 * 60 + 16)) < 3 && fabs(set - (21 * 60 + 32)) < 3); /* New York */
    CHECK(xd_sun_times(2026, 3, 20, -33.8688, 151.2093, &rise, &set) == XD_SUN_RISES_AND_SETS);
    /* Sydney: 6:59 AM and 7:08 PM AEDT (UTC+11) on the equinox. */
    CHECK(fabs(rise - (-4 * 60 - 1)) < 3 && fabs(set - (8 * 60 + 8)) < 3);
    CHECK(xd_sun_times(2026, 6, 21, 69.6492, 18.9553, &rise, &set) == XD_SUN_UP_ALL_DAY);
    CHECK(xd_sun_times(2026, 12, 21, 69.6492, 18.9553, &rise, &set) == XD_SUN_DOWN_ALL_DAY);

    /* 9 PM to 7 AM, half an hour's change centred on each. */
    CHECK(xd_nightlight_fraction(12 * 60, 1260, 420, 30) == 0.0);
    CHECK(xd_nightlight_fraction(1245, 1260, 420, 30) == 0.0);
    CHECK(xd_nightlight_fraction(1260, 1260, 420, 30) == 0.5);
    CHECK(fabs(xd_nightlight_fraction(1252.5, 1260, 420, 30) - 0.25) < 1e-9);
    CHECK(xd_nightlight_fraction(1275, 1260, 420, 30) == 1.0);
    CHECK(xd_nightlight_fraction(0, 1260, 420, 30) == 1.0);
    CHECK(xd_nightlight_fraction(1439.9, 1260, 420, 30) == 1.0);
    CHECK(xd_nightlight_fraction(420, 1260, 420, 30) == 0.5);
    CHECK(fabs(xd_nightlight_fraction(427.5, 1260, 420, 30) - 0.25) < 1e-9);
    CHECK(xd_nightlight_fraction(435, 1260, 420, 30) == 0.0);
    /* A change at midnight spreads over both days. */
    CHECK(fabs(xd_nightlight_fraction(1430, 0, 360, 30) - 1.0 / 6.0) < 1e-9);
    CHECK(fabs(xd_nightlight_fraction(5, 0, 360, 30) - 2.0 / 3.0) < 1e-9);
    /* Night in the day (a shift worker's hours), and none at all. */
    CHECK(xd_nightlight_fraction(12 * 60, 9 * 60, 17 * 60, 30) == 1.0);
    CHECK(xd_nightlight_fraction(12 * 60, 600, 600, 30) == 0.0);
    /* A night shorter than its ramps peaks without jumping. */
    CHECK(xd_nightlight_fraction(605, 600, 610, 30) == 1.0);
    CHECK(xd_nightlight_fraction(600, 600, 610, 30) == 0.5);
    /* Smooth: no step bigger than a ramp's slope allows over the day. */
    double last = xd_nightlight_fraction(0, 1260, 420, 30);
    bool smooth = true;
    for (int i = 1; i <= 2880; i++) {
        double fraction = xd_nightlight_fraction(i / 2.0, 1260, 420, 30);
        smooth = smooth && fabs(fraction - last) <= 0.5 / 30 + 1e-9;
        last = fraction;
    }
    CHECK(smooth);

    /* The schedule, in UTC. */
    const char *tz = getenv("TZ");
    char *saved_tz = tz != nullptr ? g_strdup(tz) : nullptr;
    setenv("TZ", "UTC", 1);
    tzset();
    struct tm noon = {.tm_year = 126, .tm_mon = 5, .tm_mday = 21, .tm_hour = 12};
    time_t t_noon = timegm(&noon);
    xd_nightlight_settings nl;
    xd_settings def;
    xd_settings_defaults(&def);
    nl = def.night_light;
    CHECK(xd_nightlight_kelvin(&nl, t_noon) == XD_NIGHTLIGHT_DAY); /* off */
    nl.enabled = true;
    nl.schedule = XD_NIGHTLIGHT_ALWAYS;
    CHECK(xd_nightlight_kelvin(&nl, t_noon) == 3400);
    nl.schedule = XD_NIGHTLIGHT_MANUAL;
    CHECK(xd_nightlight_kelvin(&nl, t_noon) == XD_NIGHTLIGHT_DAY);
    CHECK(xd_nightlight_kelvin(&nl, t_noon + 9 * 3600) == 4950); /* 9 PM: halfway */
    CHECK(xd_nightlight_kelvin(&nl, t_noon + 12 * 3600) == 3400);
    /* Sunset to sunrise without a location: the set hours. */
    nl.schedule = XD_NIGHTLIGHT_SUNSET;
    CHECK(xd_nightlight_kelvin(&nl, t_noon + 9 * 3600) == 4950);
    /* In London: night by 22:00 UTC on midsummer, day at 21:00 BST
     * (20:00 UTC), and changing at 20:21. */
    nl.has_location = true;
    nl.latitude = 51.5074;
    nl.longitude = -0.1278;
    CHECK(xd_nightlight_kelvin(&nl, t_noon + 10 * 3600) == 3400);
    CHECK(xd_nightlight_kelvin(&nl, t_noon + 8 * 3600) == XD_NIGHTLIGHT_DAY);
    double k = xd_nightlight_kelvin(&nl, t_noon + 8 * 3600 + 21 * 60);
    CHECK(k > 4500 && k < 5500);
    double on, off;
    CHECK(xd_nightlight_times(&nl, t_noon, &on, &off) == XD_SUN_RISES_AND_SETS);
    CHECK(fabs(on - (20 * 60 + 21)) < 3 && fabs(off - (3 * 60 + 43)) < 3);
    /* Tromsø: all day either way. */
    nl.latitude = 69.6492;
    nl.longitude = 18.9553;
    CHECK(xd_nightlight_kelvin(&nl, t_noon) == XD_NIGHTLIGHT_DAY);
    struct tm midwinter = {.tm_year = 126, .tm_mon = 11, .tm_mday = 21, .tm_hour = 12};
    CHECK(xd_nightlight_kelvin(&nl, timegm(&midwinter)) == 3400);
    if (saved_tz != nullptr)
        setenv("TZ", saved_tz, 1);
    else
        unsetenv("TZ");
    tzset();
    g_free(saved_tz);

    /* Ramps: identity, scaled, and calibration first. */
    uint16_t red[4], green[4], blue[4];
    xd_gamma_ramp_fill(red, green, blue, 4, (double[3]){1, 1, 1}, nullptr);
    CHECK(red[0] == 0 && red[1] == 21845 && red[2] == 43690 && red[3] == 65535 &&
          green[3] == 65535);
    xd_gamma_ramp_fill(red, green, blue, 4, (double[3]){1, 0.5, 0}, nullptr);
    CHECK(red[3] == 65535 && green[1] == 10923 && green[3] == 32768 && blue[3] == 0);
    const float calibration[12] = {0, 0.5f, 0.5f, 1, 0, 0, 0, 0.8f, 0, 0.25f, 0.5f, 2};
    xd_gamma_ramp_fill(red, green, blue, 4, (double[3]){1, 0.5, 1}, calibration);
    CHECK(red[1] == 32768 && red[2] == 32768 && red[3] == 65535);
    CHECK(green[2] == 0 && green[3] == 26214); /* 0.8 x 0.5 */
    CHECK(blue[3] == 65535); /* clamped */
}

/* A monitor profile's calibration curves (vcgt), through Little CMS. */
static void test_vcgt(void) {
    float out[15];
    CHECK(!xd_vcgt_load("/nonexistent/profile.icc", 5, out));
#ifdef XD_HAVE_LCMS2
    char *dir = g_dir_make_tmp("xd-vcgt-XXXXXX", nullptr);
    char *with = g_build_filename(dir, "calibrated.icc", nullptr);
    char *without = g_build_filename(dir, "plain.icc", nullptr);
    cmsHPROFILE cms_hprofile = cmsCreate_sRGBProfile();
    CHECK(cmsSaveProfileToFile(cms_hprofile, without));
    cmsToneCurve *red = cmsBuildGamma(nullptr, 2.0);
    cmsToneCurve *green = cmsBuildGamma(nullptr, 1.0);
    cmsToneCurve *blue = cmsBuildGamma(nullptr, 0.5);
    cmsToneCurve *curves[3] = {red, green, blue};
    CHECK(cmsWriteTag(cms_hprofile, cmsSigVcgtTag, curves));
    CHECK(cmsSaveProfileToFile(cms_hprofile, with));
    cmsCloseProfile(cms_hprofile);
    cmsFreeToneCurve(red);
    cmsFreeToneCurve(green);
    cmsFreeToneCurve(blue);
    CHECK(!xd_vcgt_load(without, 5, out));
    CHECK(xd_vcgt_load(with, 5, out));
    CHECK(fabsf(out[0]) < 1e-3f && fabsf(out[1] - 0.0625f) < 1e-3f &&
          fabsf(out[2] - 0.25f) < 1e-3f && fabsf(out[4] - 1.0f) < 1e-3f);
    CHECK(fabsf(out[5 + 2] - 0.5f) < 1e-3f);
    CHECK(fabsf(out[10 + 1] - 0.5f) < 1e-3f && fabsf(out[14] - 1.0f) < 1e-3f);
    g_remove(with);
    g_remove(without);
    g_rmdir(dir);
    g_free(with);
    g_free(without);
    g_free(dir);
#endif
}

/* Color Management: colord ids and names, and installing ICC files into
 * the user's store. */
static void test_color_management(void) {
    char *id = xd_color_display_id("Dell Inc.", "U2415", "7MT0", "DP-1");
    CHECK(strcmp(id, "xrandr-Dell Inc.-U2415-7MT0") == 0);
    g_free(id);
    id = xd_color_display_id("BOE", "", nullptr, "eDP-1");
    CHECK(strcmp(id, "xrandr-BOE") == 0);
    g_free(id);
    id = xd_color_display_id("", nullptr, "", "HEADLESS-1");
    CHECK(strcmp(id, "xrandr-HEADLESS-1") == 0);
    g_free(id);
    CHECK(strcmp(xd_color_kind_label("printer"), "Printer") == 0);
    CHECK(strcmp(xd_color_kind_label("plotter"), "Device") == 0);
    char *label = xd_color_device_label("printer", "HP", "HP LaserJet 4", "cups-LJ");
    CHECK(strcmp(label, "Printer: HP LaserJet 4") == 0);
    g_free(label);
    label = xd_color_device_label("display", "Dell Inc.", "U2415", "x");
    CHECK(strcmp(label, "Display: Dell Inc. U2415") == 0);
    g_free(label);
    label = xd_color_device_label("scanner", "", "", "sane-test:0");
    CHECK(strcmp(label, "Scanner: sane-test:0") == 0);
    g_free(label);

    /* A minimal profile: the header's size and signature. */
    guint8 icc[132] = {0, 0, 0, 132};
    memcpy(icc + 36, "acsp", 4);
    CHECK(xd_color_looks_like_icc(icc, sizeof(icc)));
    CHECK(!xd_color_looks_like_icc(icc, 100));
    icc[3] = 200; /* claims more than there is */
    CHECK(!xd_color_looks_like_icc(icc, sizeof(icc)));
    icc[3] = 132;

    char *dir = g_dir_make_tmp("xd-color-XXXXXX", nullptr);
    char *store = g_build_filename(dir, "share", "icc", nullptr);
    char *src = g_build_filename(dir, "Monitor.icm", nullptr);
    char *junk = g_build_filename(dir, "notes.icc", nullptr);
    CHECK(g_file_set_contents(src, (const char *)icc, sizeof(icc), nullptr));
    CHECK(g_file_set_contents(junk, "not a profile", -1, nullptr));
    char *pid = nullptr;
    char *stored = xd_color_import(src, store, &pid, nullptr);
    char *want = g_build_filename(store, "Monitor.icm", nullptr);
    CHECK(g_strcmp0(stored, want) == 0 && g_file_test(want, G_FILE_TEST_EXISTS));
    char *md5 = g_compute_checksum_for_data(G_CHECKSUM_MD5, icc, sizeof(icc));
    char *want_id = g_strdup_printf("icc-%s", md5);
    CHECK(g_strcmp0(pid, want_id) == 0);
    /* The same file again: the copy already there. */
    char *again = xd_color_import(src, store, nullptr, nullptr);
    CHECK(g_strcmp0(again, want) == 0);
    /* Another profile of that name: beside it. */
    icc[100] = 1;
    CHECK(g_file_set_contents(src, (const char *)icc, sizeof(icc), nullptr));
    char *other = xd_color_import(src, store, nullptr, nullptr);
    char *want2 = g_build_filename(store, "Monitor-2.icm", nullptr);
    CHECK(g_strcmp0(other, want2) == 0);
    GError *error = nullptr;
    CHECK(xd_color_import(junk, store, nullptr, &error) == nullptr && error != nullptr);
    g_clear_error(&error);
    CHECK(xd_color_import("/nonexistent.icc", store, nullptr, &error) == nullptr &&
          error != nullptr);
    g_clear_error(&error);
    g_remove(want);
    g_remove(want2);
    g_rmdir(store);
    char *share = g_path_get_dirname(store);
    g_rmdir(share);
    g_remove(src);
    g_remove(junk);
    g_rmdir(dir);
    g_free(share);
    g_free(want2);
    g_free(other);
    g_free(again);
    g_free(want_id);
    g_free(md5);
    g_free(want);
    g_free(stored);
    g_free(pid);
    g_free(junk);
    g_free(src);
    g_free(store);
    g_free(dir);
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
    xd_nc_state connection = {.has_device = true,
                              .device_state = 100,
                              .managed = true,
                              .autoconnect = true,
                              .carrier_known = true,
                              .carrier = true,
                              .connectivity = 4};
    CHECK(strcmp(xd_nc_status_text(&connection), "Connected") == 0 &&
          xd_nc_state_connected(&connection) && !xd_nc_state_disabled(&connection));
    connection.connectivity = 3;
    CHECK(strcmp(xd_nc_status_text(&connection), "Limited or no connectivity") == 0);
    connection.connectivity = 0; /* not checked: connected */
    CHECK(strcmp(xd_nc_status_text(&connection), "Connected") == 0);
    connection.device_state = 20;
    connection.carrier = false;
    CHECK(strcmp(xd_nc_status_text(&connection), "Network cable unplugged") == 0 &&
          !xd_nc_state_connected(&connection));
    connection.carrier_known = false; /* Wi-Fi, say */
    CHECK(strcmp(xd_nc_status_text(&connection), "Not connected") == 0);
    connection.device_state = 30;
    CHECK(strcmp(xd_nc_status_text(&connection), "Not connected") == 0 &&
          !xd_nc_state_disabled(&connection));
    connection.autoconnect = false;
    CHECK(strcmp(xd_nc_status_text(&connection), "Disabled") == 0 &&
          xd_nc_state_disabled(&connection));
    connection.autoconnect = true;
    connection.radio_off = true;
    CHECK(strcmp(xd_nc_status_text(&connection), "Disabled") == 0);
    connection.radio_off = false;
    connection.managed = false;
    connection.device_state = 10;
    CHECK(strcmp(xd_nc_status_text(&connection), "Disabled") == 0);
    connection.managed = true;
    connection.device_state = 70;
    CHECK(strcmp(xd_nc_status_text(&connection), "Acquiring network address") == 0);
    connection.device_state = 60;
    CHECK(strcmp(xd_nc_status_text(&connection), "Authenticating") == 0);
    /* A VPN goes by its active connection. */
    xd_nc_state vpn = {.active_state = 0};
    CHECK(strcmp(xd_nc_status_text(&vpn), "Disconnected") == 0 && !xd_nc_state_disabled(&vpn));
    vpn.active_state = 2;
    CHECK(strcmp(xd_nc_status_text(&vpn), "Connected") == 0 && xd_nc_state_connected(&vpn));

    char *name = xd_nc_numbered_name("Local Area Connection", 1);
    CHECK(strcmp(name, "Local Area Connection") == 0);
    g_free(name);
    name = xd_nc_numbered_name("Local Area Connection", 3);
    CHECK(strcmp(name, "Local Area Connection 3") == 0);
    g_free(name);

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
        name = xd_nc_format_duration(nums[i].n);
        CHECK(strcmp(name, nums[i].duration) == 0);
        g_free(name);
        name = xd_nc_format_count(nums[i].n);
        CHECK(strcmp(name, nums[i].count) == 0);
        g_free(name);
    }
    static const struct {
        guint64 kbps;
        const char *text;
    } speeds[] = {{1000000, "1.0 Gbps"}, {2500000, "2.5 Gbps"}, {100000, "100.0 Mbps"},
                  {54000, "54.0 Mbps"},  {866700, "866.7 Mbps"}, {56, "56.0 Kbps"}};
    for (size_t i = 0; i < XD_ARRAY_LEN(speeds); i++) {
        name = xd_nc_format_speed(speeds[i].kbps);
        CHECK(strcmp(name, speeds[i].text) == 0);
        g_free(name);
    }

    unsigned prefix = 99;
    CHECK(xd_nc_netmask_to_prefix("255.255.255.0", &prefix) && prefix == 24);
    CHECK(xd_nc_netmask_to_prefix("255.255.240.0", &prefix) && prefix == 20);
    CHECK(xd_nc_netmask_to_prefix("0.0.0.0", &prefix) && prefix == 0);
    CHECK(xd_nc_netmask_to_prefix("255.255.255.255", &prefix) && prefix == 32);
    CHECK(!xd_nc_netmask_to_prefix("255.0.255.0", &prefix) && !xd_nc_netmask_to_prefix("", &prefix) &&
          !xd_nc_netmask_to_prefix("255.255.255", &prefix) && !xd_nc_netmask_to_prefix("::", &prefix));
    for (unsigned prefix_length = 0; prefix_length <= 32; prefix_length++) {
        name = xd_nc_prefix_to_netmask(prefix_length);
        unsigned back = 99;
        CHECK(xd_nc_netmask_to_prefix(name, &back) && back == prefix_length);
        g_free(name);
    }
    name = xd_nc_prefix_to_netmask(22);
    CHECK(strcmp(name, "255.255.252.0") == 0);
    g_free(name);
    CHECK(xd_nc_valid_ipv4("10.0.0.1") && !xd_nc_valid_ipv4("10.0.0") && !xd_nc_valid_ipv4("fe80::1") &&
          !xd_nc_valid_ipv4("300.1.1.1") && !xd_nc_valid_ipv4(nullptr));
    CHECK(xd_nc_default_prefix("10.1.2.3") == 8 && xd_nc_default_prefix("172.16.0.5") == 16 &&
          xd_nc_default_prefix("192.168.1.20") == 24 && xd_nc_default_prefix("224.0.0.1") == 0 &&
          xd_nc_default_prefix("x") == 0);

    xd_nc_ipv4_form manual_form = {.manual_ip = true,
                                   .manual_dns = true,
                                   .address = "192.168.1.20",
                                   .netmask = "255.255.255.0",
                                   .gateway = "192.168.1.1",
                                   .dns1 = "1.1.1.1",
                                   .dns2 = ""};
    CHECK(xd_nc_check_ipv4_form(&manual_form) == nullptr);
    manual_form.gateway = "";
    CHECK(xd_nc_check_ipv4_form(&manual_form) == nullptr); /* no gateway is fine */
    manual_form.gateway = "192.168.2.1";
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr); /* another subnet */
    manual_form.gateway = "192.168.1.1";
    manual_form.address = "";
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr);
    manual_form.address = "192.168.1.0"; /* the network's own address */
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr);
    manual_form.address = "192.168.1.255";
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr);
    manual_form.address = "127.0.0.5";
    manual_form.netmask = "255.0.0.0";
    manual_form.gateway = "";
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr);
    manual_form.address = "192.168.1.20";
    manual_form.netmask = "255.0.255.0";
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr);
    manual_form.netmask = "255.255.255.0";
    manual_form.dns2 = "dns.example";
    CHECK(xd_nc_check_ipv4_form(&manual_form) != nullptr);
    /* Automatic: the blanks don't matter. */
    xd_nc_ipv4_form automatic_form = {
        .address = "", .netmask = "", .gateway = "", .dns1 = "", .dns2 = ""};
    CHECK(xd_nc_check_ipv4_form(&automatic_form) == nullptr);
    automatic_form.manual_dns = true;
    automatic_form.dns1 = "8.8.8.8";
    CHECK(xd_nc_check_ipv4_form(&automatic_form) == nullptr);
    automatic_form.dns1 = "8.8.8";
    CHECK(xd_nc_check_ipv4_form(&automatic_form) != nullptr);

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
    char *label = xd_unit_startup_label("enabled");
    CHECK(strcmp(label, "Enabled") == 0);
    g_free(label);
    label = xd_unit_startup_label("masked-runtime");
    CHECK(strcmp(label, "Masked (runtime)") == 0);
    g_free(label);
    label = xd_unit_startup_label(nullptr);
    CHECK(strcmp(label, "") == 0);
    g_free(label);
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

    char *name = xd_unit_display_name("sshd.service");
    CHECK(strcmp(name, "sshd") == 0);
    g_free(name);
    name = xd_unit_display_name("user@1000.service");
    CHECK(strcmp(name, "user@1000") == 0);
    g_free(name);
    name = xd_unit_display_name("noext");
    CHECK(strcmp(name, "noext") == 0);
    g_free(name);

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
    char *text = xd_disk_size_text(250059350016ULL);
    CHECK(strcmp(text, "232.88 GB") == 0);
    g_free(text);
    text = xd_disk_size_text(512 * XD_MIB);
    CHECK(strcmp(text, "512 MB") == 0);
    g_free(text);
    text = xd_disk_size_text(4096);
    CHECK(strcmp(text, "4 KB") == 0);
    g_free(text);
    text = xd_disk_size_text(2ULL * 1024 * 1024 * XD_MIB);
    CHECK(strcmp(text, "2.00 TB") == 0);
    g_free(text);
    text = xd_disk_fs_label("vfat", "FAT32");
    CHECK(strcmp(text, "FAT32") == 0);
    g_free(text);
    text = xd_disk_fs_label("vfat", "FAT16");
    CHECK(strcmp(text, "FAT") == 0);
    g_free(text);
    text = xd_disk_fs_label("iso9660", nullptr);
    CHECK(strcmp(text, "CDFS") == 0);
    g_free(text);
    text = xd_disk_fs_label("ext4", "1.0");
    CHECK(strcmp(text, "ext4") == 0);
    g_free(text);
    text = xd_disk_fs_label("", nullptr);
    CHECK(strcmp(text, "") == 0);
    g_free(text);
    CHECK(xd_disk_percent_free(45, 100) == 45);
    CHECK(xd_disk_percent_free(1, 3) == 33 && xd_disk_percent_free(2, 3) == 67);
    CHECK(xd_disk_percent_free(5, 0) == 0 && xd_disk_percent_free(7, 5) == 100);

    CHECK(xd_disk_align_up(0) == 0 && xd_disk_align_up(1) == XD_MIB);
    CHECK(xd_disk_align_up(XD_MIB) == XD_MIB && xd_disk_align_down(XD_MIB + 5) == XD_MIB);

    /* An 8 GiB MBR disk: a 2 GiB primary partition, a 4 GiB extended
     * one holding a 1 GiB logical drive, and 2 GiB never used. */
    const guint64 gib = 1024 * XD_MIB;
    const xd_disk_part parts[] = {
        {.offset = 2 * gib + XD_MIB, .size = 4 * gib, .index = 1, .container = true},
        {.offset = XD_MIB, .size = 2 * gib, .index = 0},
        {.offset = 2 * gib + 2 * XD_MIB, .size = gib, .index = 2},
    };
    GArray *segs = xd_disk_segments(8 * gib, parts, G_N_ELEMENTS(parts));
    CHECK(segs->len == 4);
    if (segs->len == 4) {
        const xd_disk_segment *disk_segment = &g_array_index(segs, xd_disk_segment, 0);
        CHECK(disk_segment[0].kind == XD_SEGMENT_PARTITION && disk_segment[0].index == 0 &&
              !disk_segment[0].logical);
        CHECK(disk_segment[1].kind == XD_SEGMENT_PARTITION && disk_segment[1].index == 2 &&
              disk_segment[1].logical);
        CHECK(disk_segment[2].kind == XD_SEGMENT_FREE && disk_segment[2].logical &&
              disk_segment[2].offset == 3 * gib + 2 * XD_MIB &&
              disk_segment[2].size == 3 * gib - XD_MIB);
        CHECK(disk_segment[3].kind == XD_SEGMENT_UNALLOCATED &&
              disk_segment[3].offset == 6 * gib + XD_MIB &&
              disk_segment[3].size == 2 * gib - XD_MIB);
        /* The logical drive grows into the extended partition's free
         * space; the primary one has a neighbour. */
        CHECK(xd_disk_grow_max(segs, 1, 8 * gib) == 4 * gib - XD_MIB);
        CHECK(xd_disk_grow_max(segs, 0, 8 * gib) == 2 * gib);
        guint64 off = 0, max = 0;
        CHECK(xd_disk_new_part_range(disk_segment[2].offset, disk_segment[2].size, 8 * gib, true,
                                     &off, &max));
        CHECK(off == 3 * gib + 3 * XD_MIB && max == 3 * gib - 2 * XD_MIB);
        CHECK(xd_disk_new_part_range(disk_segment[3].offset, disk_segment[3].size, 8 * gib, false,
                                     &off, &max));
        CHECK(off == 6 * gib + XD_MIB && max == 2 * gib - 2 * XD_MIB);
    }
    g_array_unref(segs);
    /* An empty disk is one unallocated block; overlaps are clipped. */
    segs = xd_disk_segments(4 * gib, nullptr, 0);
    CHECK(segs->len == 1 &&
          g_array_index(segs, xd_disk_segment, 0).kind == XD_SEGMENT_UNALLOCATED);
    g_array_unref(segs);
    const xd_disk_part overlap[] = {{.offset = XD_MIB, .size = 2 * gib, .index = 0},
                                    {.offset = gib, .size = 4 * gib, .index = 1}};
    segs = xd_disk_segments(4 * gib, overlap, 2);
    CHECK(segs->len == 2 && g_array_index(segs, xd_disk_segment, 1).offset == 2 * gib + XD_MIB &&
          g_array_index(segs, xd_disk_segment, 1).size == 2 * gib - XD_MIB);
    g_array_unref(segs);
    guint64 off = 0, max = 0;
    CHECK(xd_disk_new_part_range(0, 4 * gib, 4 * gib, false, &off, &max) && off == XD_MIB &&
          max == 4 * gib - 2 * XD_MIB);
    CHECK(!xd_disk_new_part_range(XD_MIB, XD_MIB / 2, 4 * gib, false, &off, &max));

    xd_new_part_kinds new_part_kinds =
        xd_disk_new_part_kinds("dos", XD_SEGMENT_UNALLOCATED, 2, true);
    CHECK(new_part_kinds.primary && !new_part_kinds.extended && !new_part_kinds.logical);
    new_part_kinds = xd_disk_new_part_kinds("dos", XD_SEGMENT_UNALLOCATED, 1, false);
    CHECK(new_part_kinds.primary && new_part_kinds.extended && !new_part_kinds.logical);
    new_part_kinds = xd_disk_new_part_kinds("dos", XD_SEGMENT_UNALLOCATED, 4, false);
    CHECK(!new_part_kinds.primary && !new_part_kinds.extended && !new_part_kinds.logical);
    new_part_kinds = xd_disk_new_part_kinds("dos", XD_SEGMENT_FREE, 4, true);
    CHECK(!new_part_kinds.primary && !new_part_kinds.extended && new_part_kinds.logical);
    new_part_kinds = xd_disk_new_part_kinds("gpt", XD_SEGMENT_UNALLOCATED, 10, false);
    CHECK(new_part_kinds.primary && !new_part_kinds.extended && !new_part_kinds.logical);
    new_part_kinds = xd_disk_new_part_kinds("gpt", XD_SEGMENT_PARTITION, 1, false);
    CHECK(!new_part_kinds.primary);

    CHECK(xd_disk_shrink_min(0, 10 * gib) == 32 * XD_MIB);
    CHECK(xd_disk_shrink_min(gib, 10 * gib) == gib + 103 * XD_MIB);
    CHECK(xd_disk_shrink_min(10 * XD_MIB, 10 * gib) == 74 * XD_MIB);
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
    xd_volume_use volume_use = {.mounts = root, .id_type = "ext4"};
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_FORMAT, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_SHRINK, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_EXTEND, &unmount) == nullptr && !unmount);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_LABEL, &unmount) == nullptr);
    volume_use.mounts = media;
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_FORMAT, &unmount) == nullptr && unmount);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_SET_TYPE, &unmount) == nullptr && !unmount);
    volume_use = (xd_volume_use){.id_type = "swap", .swap_active = true};
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_LABEL, &unmount) != nullptr);
    volume_use.swap_active = false;
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) == nullptr && !unmount);
    volume_use = (xd_volume_use){.container = true, .has_logical = true};
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) != nullptr);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_FORMAT, &unmount) != nullptr);
    volume_use.has_logical = false;
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) == nullptr);
    volume_use = (xd_volume_use){.id_type = "LVM2_member"};
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_FORMAT, &unmount) != nullptr);
    volume_use = (xd_volume_use){.id_type = "crypto_LUKS", .unlocked = true};
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) != nullptr);
    volume_use.unlocked = false;
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_DELETE, &unmount) == nullptr);
    CHECK(xd_volume_lock_reason(&volume_use, XD_ACTION_EXTEND, &unmount) != nullptr);

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
    int widths[3] = {0};
    xd_disk_block_widths(sizes, 3, 400, 50, widths);
    CHECK(widths[0] + widths[1] + widths[2] == 400 && widths[0] >= 50 && widths[2] > widths[0] &&
          widths[0] == widths[1]);
    xd_disk_block_widths(sizes, 3, 100, 50, widths);
    CHECK(widths[0] == 50 && widths[1] == 50 && widths[2] == 50);

    /* Startup mount points and format options. */
    CHECK(xd_udisks_version_at_least("2.11.0", 2, 10) && xd_udisks_version_at_least("3", 2, 10));
    CHECK(!xd_udisks_version_at_least("2.9.4", 2, 10) && !xd_udisks_version_at_least("", 2, 10));
    char *dir = xd_fstab_dir_clean("  /mnt/data//  ");
    CHECK(strcmp(dir, "/mnt/data") == 0);
    g_free(dir);
    dir = xd_fstab_dir_clean("/");
    CHECK(strcmp(dir, "/") == 0);
    g_free(dir);
    CHECK(xd_fstab_dir_problem("/mnt/data") == nullptr);
    CHECK(xd_fstab_dir_problem("/srv") != nullptr && xd_fstab_dir_problem("/srv/media") == nullptr);
    CHECK(xd_fstab_dir_problem("/data/media files") == nullptr);
    CHECK(xd_fstab_dir_problem("") != nullptr && xd_fstab_dir_problem("mnt/data") != nullptr);
    CHECK(xd_fstab_dir_problem("/mnt/../etc") != nullptr);
    CHECK(xd_fstab_dir_problem("/mnt/a\nb") != nullptr);
    CHECK(xd_fstab_dir_problem("/") != nullptr && xd_fstab_dir_problem("/home") != nullptr);
    CHECK(xd_fstab_dir_problem("/proc/x") != nullptr && xd_fstab_dir_problem("/usr/local") != nullptr);
    CHECK(xd_fstab_dir_problem("/mnt") != nullptr && xd_fstab_dir_problem("/procs") == nullptr);
    dir = xd_fstab_suggest_dir("My Data", "/dev/sdb1");
    CHECK(strcmp(dir, "/mnt/My_Data") == 0);
    g_free(dir);
    dir = xd_fstab_suggest_dir("", "/dev/sdb1");
    CHECK(strcmp(dir, "/mnt/sdb1") == 0);
    g_free(dir);
    dir = xd_fstab_suggest_dir("..", nullptr);
    CHECK(strcmp(dir, "/mnt/volume") == 0);
    g_free(dir);
    char *opts = xd_fstab_default_options("ext4", 1000, 1000);
    CHECK(strcmp(opts, "nofail,x-mount.mkdir") == 0);
    g_free(opts);
    opts = xd_fstab_default_options("vfat", 1000, 100);
    CHECK(strcmp(opts, "nofail,x-mount.mkdir,uid=1000,gid=100") == 0);
    g_free(opts);

    CHECK(xd_format_unit_sizes("btrfs") == nullptr && xd_format_unit_sizes("swap") == nullptr);
    CHECK(xd_format_unit_sizes("ntfs")[7] == 65536 && xd_format_unit_sizes("ntfs")[8] == 0);
    char *ul = xd_format_unit_label(4096);
    CHECK(strcmp(ul, "4096 bytes") == 0);
    g_free(ul);
    ul = xd_format_unit_label(65536);
    CHECK(strcmp(ul, "64K") == 0);
    g_free(ul);
    char **args = xd_format_unit_args("ext4", 4096, 512);
    CHECK(args != nullptr && strcmp(args[0], "-b") == 0 && strcmp(args[1], "4096") == 0 &&
          args[2] == nullptr);
    g_strfreev(args);
    args = xd_format_unit_args("vfat", 32768, 512);
    CHECK(args != nullptr && strcmp(args[0], "-s") == 0 && strcmp(args[1], "64") == 0);
    g_strfreev(args);
    args = xd_format_unit_args("xfs", 2048, 512);
    CHECK(args != nullptr && strcmp(args[1], "size=2048") == 0);
    g_strfreev(args);
    args = xd_format_unit_args("exfat", 131072, 4096);
    CHECK(args != nullptr && strcmp(args[0], "-c") == 0 && strcmp(args[1], "131072") == 0);
    g_strfreev(args);
    CHECK(xd_format_unit_args("ntfs", 512, 4096) == nullptr); /* smaller than a sector */
    CHECK(xd_format_unit_args("ext4", 8192, 512) == nullptr); /* not offered */
    CHECK(xd_format_unit_args("btrfs", 4096, 512) == nullptr);
}

/* Flatpak permissions: merging layers as flatpak does (checked against
 * `flatpak info --show-permissions`), and the user override written in
 * `flatpak override --user`'s format. */
static void fp_merge(xd_fp_perms *permissions, const char *text) {
    GKeyFile *kf = g_key_file_new();
    CHECK(g_key_file_load_from_data(kf, text, (gsize)-1, G_KEY_FILE_NONE, nullptr));
    xd_fp_perms_merge(permissions, kf);
    g_key_file_free(kf);
}

static void test_flatpak_perms(void) {
    xd_fp_perms *permissions = xd_fp_perms_new();
    fp_merge(permissions,
             "[Context]\nsockets=x11;if:x11:!has-wayland;pulseaudio;\n"
             "devices=dri;\nfilesystems=xdg-download:ro;home/Games:create;/srv//data/;\n"
             "persistent=.hello;\nunset-environment=GONE;\n"
             "[Session Bus Policy]\norg.freedesktop.Notifications=talk\n"
             "[Environment]\nHELLO_MODE=fast\nGONE=\n");
    /* A plain "x11" beside conditions is for older Flatpaks. */
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "x11") == XD_FP_CONDITIONAL);
    const GPtrArray *conds = xd_fp_perms_conditions(permissions, XD_FP_SOCKETS, "x11");
    CHECK(conds != nullptr && conds->len == 1 &&
          strcmp(g_ptr_array_index(conds, 0), "!has-wayland") == 0);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "pulseaudio") == XD_FP_ON);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "cups") == XD_FP_UNSET);
    bool found;
    CHECK(xd_fp_perms_filesystem(permissions, "xdg-download", &found) == XD_FP_FS_RO && found);
    CHECK(xd_fp_perms_filesystem(permissions, "~/Games", &found) == XD_FP_FS_CREATE && found);
    CHECK(xd_fp_perms_filesystem(permissions, "/srv/data", &found) == XD_FP_FS_RW && found);
    xd_fp_perms_filesystem(permissions, "home", &found);
    CHECK(!found);
    CHECK(g_strcmp0(xd_fp_perms_env(permissions, "HELLO_MODE", &found), "fast") == 0 && found);
    CHECK(xd_fp_perms_env(permissions, "GONE", &found) == nullptr && found);
    xd_fp_perms_env(permissions, "OTHER", &found);
    CHECK(!found);
    CHECK(
        g_strcmp0(xd_fp_perms_bus(permissions, XD_FP_SESSION_BUS, "org.freedesktop.Notifications"),
                  "talk") == 0);
    CHECK(xd_fp_perms_bus(permissions, XD_FP_SYSTEM_BUS, "org.freedesktop.Notifications") ==
          nullptr);

    /* A layer above changes only what it mentions; "!x" also ends a lower
     * layer's conditions; a reset drops the filesystems below. */
    xd_fp_perms *base = xd_fp_perms_copy(permissions);
    fp_merge(permissions, "[Context]\nsockets=!x11;cups;!pulseaudio;newsock;\n"
                          "filesystems=!host:reset;!host;home:ro;\npersistent=.more;\n"
                          "[System Bus Policy]\norg.bluez=talk\n"
                          "[Session Bus Policy]\norg.freedesktop.Notifications=none\n");
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "x11") == XD_FP_OFF);
    CHECK(xd_fp_perms_conditions(permissions, XD_FP_SOCKETS, "x11") == nullptr);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "cups") == XD_FP_ON);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "pulseaudio") == XD_FP_OFF);
    CHECK(xd_fp_perms_state(permissions, XD_FP_DEVICES, "dri") == XD_FP_ON);
    GPtrArray *other = xd_fp_perms_other_names(permissions, XD_FP_SOCKETS);
    CHECK(other->len == 1 && strcmp(g_ptr_array_index(other, 0), "newsock") == 0);
    g_ptr_array_unref(other);
    GPtrArray *fs = xd_fp_perms_filesystems(permissions);
    CHECK(fs->len == 2 && strcmp(g_ptr_array_index(fs, 0), "home") == 0 &&
          strcmp(g_ptr_array_index(fs, 1), "host") == 0);
    g_ptr_array_unref(fs);
    CHECK(xd_fp_perms_filesystem(permissions, "~", &found) == XD_FP_FS_RO && found);
    CHECK(xd_fp_perms_filesystem(permissions, "host", &found) == XD_FP_FS_NONE && found);
    GPtrArray *dirs = xd_fp_perms_persistent(permissions);
    CHECK(dirs->len == 2);
    g_ptr_array_unref(dirs);
    CHECK(
        g_strcmp0(xd_fp_perms_bus(permissions, XD_FP_SESSION_BUS, "org.freedesktop.Notifications"),
                  "none") == 0);
    CHECK(g_strcmp0(xd_fp_perms_bus(permissions, XD_FP_SYSTEM_BUS, "org.bluez"), "talk") == 0);
    /* The copy kept what was there before. */
    CHECK(xd_fp_perms_state(base, XD_FP_SOCKETS, "x11") == XD_FP_CONDITIONAL);
    CHECK(xd_fp_perms_filesystem(base, "xdg-download", &found) == XD_FP_FS_RO && found);
    xd_fp_perms_free(base);
    xd_fp_perms_free(permissions);

    /* Names. */
    xd_fp_fs_mode mode;
    char *canon = xd_fp_filesystem_parse("!~/", &mode);
    CHECK(g_strcmp0(canon, "home") == 0 && mode == XD_FP_FS_NONE);
    g_free(canon);
    canon = xd_fp_filesystem_parse("xdg-config/app/./x//:rw", &mode);
    CHECK(g_strcmp0(canon, "xdg-config/app/x") == 0 && mode == XD_FP_FS_RW);
    g_free(canon);
    canon = xd_fp_filesystem_parse("/:ro", &mode);
    CHECK(g_strcmp0(canon, "/") == 0 && mode == XD_FP_FS_RO);
    g_free(canon);
    CHECK(xd_fp_filesystem_parse("", &mode) == nullptr);
    CHECK(xd_fp_filesystem_valid("home") && xd_fp_filesystem_valid("xdg-download/Games"));
    CHECK(xd_fp_filesystem_valid("~/x") && xd_fp_filesystem_valid("/mnt"));
    CHECK(xd_fp_filesystem_valid("host-etc") && !xd_fp_filesystem_valid("host/x"));
    CHECK(xd_fp_filesystem_valid("xdg-run/pipewire-0") && !xd_fp_filesystem_valid("xdg-run"));
    CHECK(!xd_fp_filesystem_valid("Documents") && !xd_fp_filesystem_valid(""));
    CHECK(xd_fp_bus_name_valid("org.example.App") && xd_fp_bus_name_valid("org.example.*"));
    CHECK(!xd_fp_bus_name_valid("org") && !xd_fp_bus_name_valid(":1.5") &&
          !xd_fp_bus_name_valid("org..x"));
    CHECK(xd_fp_env_name_valid("GTK_THEME") && xd_fp_env_name_valid("_X1"));
    CHECK(!xd_fp_env_name_valid("1X") && !xd_fp_env_name_valid("A=B") &&
          !xd_fp_env_name_valid(""));

    /* Editing: what `flatpak override --user --share=ipc --unshare=network
     * --nosocket=wayland --socket=x11 --filesystem=home:ro
     * --nofilesystem=xdg-download --filesystem=/srv/data:create
     * --persist=.foo --env=A=1 --unset-env=HELLO_MODE --talk-name=org.a.B
     * --no-talk-name=org.freedesktop.Notifications
     * --system-talk-name=org.bluez` writes, with an entry made by hand
     * kept and a conditional one replaced. */
    char *dir = g_dir_make_tmp("xd-flatpak-XXXXXX", nullptr);
    char *path = xd_fp_override_path(dir, "org.xd.Test");
    char *expect_path = g_build_filename(dir, "overrides", "org.xd.Test", nullptr);
    CHECK(strcmp(path, expect_path) == 0);
    g_free(expect_path);
    GKeyFile *ov = xd_fp_override_load(path, nullptr);
    CHECK(ov != nullptr);
    g_key_file_set_string(ov, "Context", "sockets", "x11;if:x11:!has-wayland;ssh-auth;");
    xd_fp_override_set_state(ov, XD_FP_SHARED, "ipc", XD_FP_ON);
    xd_fp_override_set_state(ov, XD_FP_SHARED, "network", XD_FP_OFF);
    xd_fp_override_set_state(ov, XD_FP_SOCKETS, "wayland", XD_FP_OFF);
    xd_fp_override_set_state(ov, XD_FP_SOCKETS, "x11", XD_FP_ON);
    xd_fp_override_set_filesystem(ov, "home:ro", XD_FP_FS_RO, false);
    xd_fp_override_set_filesystem(ov, "xdg-download", XD_FP_FS_NONE, false);
    xd_fp_override_set_filesystem(ov, "/srv/data", XD_FP_FS_CREATE, false);
    xd_fp_override_set_filesystem(ov, "/tmp/gone", XD_FP_FS_RW, false);
    xd_fp_override_set_filesystem(ov, "/tmp/gone/", XD_FP_FS_RW, true);
    xd_fp_override_set_persistent(ov, ".foo", true);
    xd_fp_override_set_persistent(ov, ".foo", true);
    xd_fp_override_set_env(ov, "A", "1", false);
    xd_fp_override_set_env(ov, "HELLO_MODE", nullptr, false);
    xd_fp_override_set_bus(ov, XD_FP_SESSION_BUS, "org.a.B", "talk");
    xd_fp_override_set_bus(ov, XD_FP_SESSION_BUS, "org.freedesktop.Notifications", "none");
    xd_fp_override_set_bus(ov, XD_FP_SYSTEM_BUS, "org.bluez", "talk");
    xd_fp_override_set_state(ov, XD_FP_DEVICES, "kvm", XD_FP_ON);
    xd_fp_override_set_state(ov, XD_FP_DEVICES, "kvm", XD_FP_UNSET);
    CHECK(xd_fp_override_save(ov, path, nullptr));
    g_key_file_free(ov);
    char *text = nullptr;
    CHECK(g_file_get_contents(path, &text, nullptr, nullptr));
    CHECK(g_strcmp0(text, "[Context]\n"
                          "sockets=ssh-auth;!wayland;x11;\n"
                          "shared=ipc;!network;\n"
                          "filesystems=home:ro;!xdg-download;/srv/data:create;\n"
                          "persistent=.foo;\n"
                          "unset-environment=HELLO_MODE;\n"
                          "\n"
                          "[Environment]\n"
                          "A=1\n"
                          "HELLO_MODE=\n"
                          "\n"
                          "[Session Bus Policy]\n"
                          "org.a.B=talk\n"
                          "org.freedesktop.Notifications=none\n"
                          "\n"
                          "[System Bus Policy]\n"
                          "org.bluez=talk\n") == 0);
    g_free(text);

    /* Read back as the top layer. */
    ov = xd_fp_override_load(path, nullptr);
    permissions = xd_fp_perms_new();
    fp_merge(permissions,
             "[Context]\nshared=network;\nsockets=wayland;\n[Environment]\nHELLO_MODE=fast\n");
    xd_fp_perms_merge(permissions, ov);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SHARED, "network") == XD_FP_OFF);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "wayland") == XD_FP_OFF);
    CHECK(xd_fp_perms_state(permissions, XD_FP_SOCKETS, "x11") == XD_FP_ON);
    CHECK(xd_fp_perms_env(permissions, "HELLO_MODE", &found) == nullptr && found);
    xd_fp_perms_free(permissions);

    /* Taking everything back out leaves no file, as `--reset` does. */
    xd_fp_override_set_env(ov, "HELLO_MODE", nullptr, true);
    xd_fp_override_set_env(ov, "A", nullptr, true);
    xd_fp_override_set_bus(ov, XD_FP_SESSION_BUS, "org.a.B", nullptr);
    xd_fp_override_set_bus(ov, XD_FP_SESSION_BUS, "org.freedesktop.Notifications", nullptr);
    xd_fp_override_set_bus(ov, XD_FP_SYSTEM_BUS, "org.bluez", nullptr);
    xd_fp_override_set_persistent(ov, ".foo", false);
    for (const char *const *name = xd_fp_list_names(XD_FP_SOCKETS); *name != nullptr; name++)
        xd_fp_override_set_state(ov, XD_FP_SOCKETS, *name, XD_FP_UNSET);
    xd_fp_override_set_state(ov, XD_FP_SHARED, "ipc", XD_FP_UNSET);
    xd_fp_override_set_state(ov, XD_FP_SHARED, "network", XD_FP_UNSET);
    CHECK(xd_fp_override_save(ov, path, nullptr));
    CHECK(g_file_test(path, G_FILE_TEST_EXISTS)); /* the filesystems are left */
    xd_fp_override_set_filesystem(ov, "~", XD_FP_FS_NONE, true);
    xd_fp_override_set_filesystem(ov, "xdg-download", XD_FP_FS_NONE, true);
    xd_fp_override_set_filesystem(ov, "/srv/data", XD_FP_FS_NONE, true);
    CHECK(xd_fp_override_save(ov, path, nullptr));
    CHECK(!g_file_test(path, G_FILE_TEST_EXISTS));
    CHECK(xd_fp_override_save(ov, path, nullptr)); /* nothing to remove: fine */
    g_key_file_free(ov);
    /* A file that isn't a key file is an error, not an empty override. */
    CHECK(g_file_set_contents(path, "not [a key file\n", -1, nullptr));
    GError *error = nullptr;
    CHECK(xd_fp_override_load(path, &error) == nullptr && error != nullptr);
    g_clear_error(&error);
    g_free(path);
    rm_tree(dir);
    g_free(dir);
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
    char *text = xd_appwiz_format_size(23477862);
    CHECK(strcmp(text, "22.39MB") == 0);
    g_free(text);
    text = xd_appwiz_format_size(100);
    CHECK(strcmp(text, "0.01MB") == 0);
    g_free(text);
    text = xd_appwiz_format_size(1288490189);
    CHECK(strcmp(text, "1.20GB") == 0);
    g_free(text);
    text = xd_appwiz_format_date(0);
    CHECK(strcmp(text, "") == 0);
    g_free(text);
    text = xd_appwiz_format_date(1160000000);
    CHECK(strstr(text, "06") != nullptr);
    g_free(text);

    /* Sort orders. */
    xd_appwiz_key alpha = {"Alpha", 100, 1, 50, "pacman (extra)"},
                  beta = {"beta", 300, 1, 0, "Flatpak (flathub, system)"},
                  gamma = {"Gamma", 0, 9, 70, nullptr},
                  delta = {"delta", 100, 0, 50, "pacman (extra)"};
    CHECK(xd_appwiz_compare(&alpha, &beta, XD_SORT_NAME) < 0);
    CHECK(xd_appwiz_compare(&beta, &gamma, XD_SORT_NAME) < 0); /* case doesn't matter */
    CHECK(xd_appwiz_compare(&beta, &alpha, XD_SORT_SIZE) < 0); /* largest first */
    CHECK(xd_appwiz_compare(&alpha, &gamma, XD_SORT_SIZE) < 0); /* unknown last */
    CHECK(xd_appwiz_compare(&alpha, &delta, XD_SORT_SIZE) < 0); /* ties by name */
    CHECK(xd_appwiz_compare(&gamma, &alpha, XD_SORT_FREQUENCY) < 0);
    CHECK(xd_appwiz_compare(&alpha, &beta, XD_SORT_FREQUENCY) < 0);
    CHECK(xd_appwiz_compare(&gamma, &alpha, XD_SORT_DATE) < 0); /* most recent first */
    CHECK(xd_appwiz_compare(&delta, &beta, XD_SORT_DATE) < 0); /* never used last */
    CHECK(xd_appwiz_compare(&alpha, &alpha, XD_SORT_DATE) == 0);
    CHECK(xd_appwiz_compare(&beta, &alpha, XD_SORT_SOURCE) < 0); /* by source */
    CHECK(xd_appwiz_compare(&alpha, &delta, XD_SORT_SOURCE) < 0); /* then by name */
    CHECK(xd_appwiz_compare(&delta, &gamma, XD_SORT_SOURCE) < 0); /* not known last */

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
/* Unity's LauncherEntry: app URIs, Update's properties (each changing only
 * what it names), and which taskbar buttons a program's state is for. */
static void test_launcher_entry(void) {
    char *id = xd_launcher_desktop_id("application://firefox.desktop");
    CHECK(id != nullptr && strcmp(id, "firefox.desktop") == 0);
    g_free(id);
    id = xd_launcher_desktop_id("application://org.kde.dolphin");
    CHECK(id != nullptr && strcmp(id, "org.kde.dolphin.desktop") == 0);
    g_free(id);
    CHECK(xd_launcher_desktop_id("file:///usr/share/applications/firefox.desktop") == nullptr);
    CHECK(xd_launcher_desktop_id("application://") == nullptr);
    CHECK(xd_launcher_desktop_id(nullptr) == nullptr);

    xd_launcher_state st = {0};
    GVariant *variant = g_variant_ref_sink(
        g_variant_new_parsed("{'progress': <0.5>, 'progress-visible': <true>, 'count': <int64 3>, "
                             "'count-visible': <true>, 'urgent': <true>}"));
    CHECK(xd_launcher_apply(&st, variant));
    CHECK(st.progress == 0.5 && st.progress_visible && st.count == 3 && st.count_visible &&
          st.urgent);
    CHECK(!xd_launcher_apply(&st, variant)); /* nothing new */
    g_variant_unref(variant);
    /* Only what it names; out of range clamps; wrong types are ignored. */
    variant = g_variant_ref_sink(g_variant_new_parsed(
        "{'progress': <1.5>, 'urgent': <'yes'>, 'count': <int32 7>, 'other': <1>}"));
    CHECK(xd_launcher_apply(&st, variant));
    CHECK(st.progress == 1.0 && st.progress_visible && st.count == 7 && st.urgent);
    g_variant_unref(variant);
    variant = g_variant_ref_sink(g_variant_new_parsed(
        "{'progress': <-1.0>, 'progress-visible': <false>, 'count': <'9'>, 'urgent': <false>}"));
    CHECK(xd_launcher_apply(&st, variant));
    CHECK(st.progress == 0 && !st.progress_visible && st.count == 7 && !st.urgent);
    g_variant_unref(variant);
    variant = g_variant_ref_sink(g_variant_new_parsed("{'progress': <1>}"));
    CHECK(!xd_launcher_apply(&st, variant)); /* progress is a double */
    g_variant_unref(variant);
    variant = g_variant_ref_sink(g_variant_new_parsed("('notadict', 1)"));
    CHECK(!xd_launcher_apply(&st, variant));
    g_variant_unref(variant);

    CHECK(xd_launcher_key_is("firefox.desktop", "firefox.desktop"));
    CHECK(xd_launcher_key_is("firefox.desktop", "app:firefox"));
    CHECK(!xd_launcher_key_is("firefox.desktop", "app:firefox-esr"));
    CHECK(!xd_launcher_key_is("firefox.desktop", "app:firefo"));
    CHECK(!xd_launcher_key_is("firefox.desktop", "thunderbird.desktop"));
    CHECK(!xd_launcher_key_is("firefox.desktop", "win:12"));
    CHECK(!xd_launcher_key_is("x", "app:"));

    char buf[8];
    xd_launcher_count_text(3, buf, sizeof(buf));
    CHECK(strcmp(buf, "3") == 0);
    xd_launcher_count_text(99, buf, sizeof(buf));
    CHECK(strcmp(buf, "99") == 0);
    xd_launcher_count_text(1000, buf, sizeof(buf));
    CHECK(strcmp(buf, "99+") == 0);
}

/* The clock flyout's calendar: a month's page from either first weekday,
 * always with some of the month before; days and months moved, a month's
 * end kept within the shorter month. */
/* A Notify call's parameters, as a program sends them. */
static GVariant *notify_call(const char *summary, const char *body, uint32_t replaces_id,
                             GVariant *hints) {
    static const char *const actions[] = {"default", "Open", "reply", "Reply", "odd"};
    return g_variant_ref_sink(g_variant_new("(susss^as@a{sv}i)", "Mail", replaces_id, "mail-unread",
                                            summary, body, actions, hints, -1));
}

/* Notifications: what's kept of a Notify call, replacing, the history's
 * limit and file, and markup taken out. */
/* The colour of a pixel of an RGB24 picture, as 0xRRGGBB. */
static uint32_t snip_pixel(cairo_surface_t *image, int x, int y) {
    cairo_surface_flush(image);
    const unsigned char *data = cairo_image_surface_get_data(image);
    int stride = cairo_image_surface_get_stride(image);
    return *(const uint32_t *)(const void *)(data + (size_t)y * (size_t)stride + (size_t)x * 4) &
           0xffffffu;
}

static void test_snipping(void) {
    xd_snip_mode mode = XD_SNIP_FREE_FORM;
    CHECK(xd_snip_mode_parse("window", &mode) && mode == XD_SNIP_WINDOW);
    CHECK(!xd_snip_mode_parse("lasso", &mode) && mode == XD_SNIP_WINDOW);
    CHECK(strcmp(xd_snip_mode_name(XD_SNIP_FULL_SCREEN), "full-screen") == 0);

    xd_snip_box box = xd_snip_box_from_corners((xd_snip_point){30, 40}, (xd_snip_point){10, 5});
    CHECK(box.x == 10 && box.y == 5 && box.width == 20 && box.height == 35);
    CHECK(xd_snip_box_is_selection(box));
    CHECK(!xd_snip_box_is_selection(
        xd_snip_box_from_corners((xd_snip_point){3, 3}, (xd_snip_point){4, 40})));
    const xd_snip_point line[] = {{5, 9}, {2, 4}, {8, 1}};
    box = xd_snip_box_around(line, 3);
    CHECK(box.x == 2 && box.y == 1 && box.width == 6 && box.height == 8);

    /* Frontmost first: the dialog over the big window wins. */
    const xd_snip_window windows[] = {
        {.x = 50, .y = 50, .width = 20, .height = 20, .activated = true},
        {.x = 0, .y = 0, .width = 100, .height = 100},
    };
    CHECK(xd_snip_window_at(windows, 2, 55, 55) == 0);
    CHECK(xd_snip_window_at(windows, 2, 10, 10) == 1);
    CHECK(xd_snip_window_at(windows, 2, 100, 10) == -1); /* right edge is outside */
    CHECK(xd_snip_window_active(windows, 2) == 0);
    CHECK(xd_snip_window_active(&windows[1], 1) == -1);

    /* A 2x-scale desktop of 50x40 logical px whose top left is at
     * (-50, 0): left half blue, right half green. */
    xd_snip_screen *screen = g_new0(xd_snip_screen, 1);
    screen->origin_x = -50;
    screen->scale = 2;
    screen->image = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 100, 80);
    cairo_t *cairo = cairo_create(screen->image);
    cairo_set_source_rgb(cairo, 0, 0, 1);
    cairo_rectangle(cairo, 0, 0, 50, 80);
    cairo_fill(cairo);
    cairo_set_source_rgb(cairo, 0, 1, 0);
    cairo_rectangle(cairo, 50, 0, 50, 80);
    cairo_fill(cairo);
    cairo_destroy(cairo);

    cairo_rectangle_int_t pixels;
    CHECK(xd_snip_screen_pixels(screen, (xd_snip_box){-40, 5, 10, 10}, &pixels));
    CHECK(pixels.x == 20 && pixels.y == 10 && pixels.width == 20 && pixels.height == 20);
    /* Partly off the desktop: cut to it. */
    CHECK(xd_snip_screen_pixels(screen, (xd_snip_box){-60, -10, 20, 20}, &pixels));
    CHECK(pixels.x == 0 && pixels.y == 0 && pixels.width == 20 && pixels.height == 20);
    CHECK(!xd_snip_screen_pixels(screen, (xd_snip_box){10, 0, 5, 5}, &pixels));

    /* Across the middle, with a 2 px red edge (4 pixels at 2x). */
    const double red[3] = {1, 0, 0};
    cairo_surface_t *snip =
        xd_snip_screen_cut(screen, (xd_snip_box){-35, 0, 20, 20}, nullptr, 0, red);
    CHECK(snip != nullptr && cairo_image_surface_get_width(snip) == 40 &&
          cairo_image_surface_get_height(snip) == 40);
    CHECK(snip_pixel(snip, 10, 20) == 0x0000ff && snip_pixel(snip, 30, 20) == 0x00ff00);
    CHECK(snip_pixel(snip, 1, 20) == 0xff0000 && snip_pixel(snip, 20, 38) == 0xff0000);
    cairo_surface_destroy(snip);
    /* Free-form: a triangle; outside it is white, and no ink. */
    const xd_snip_point triangle[] = {{-50, 0}, {-30, 0}, {-50, 20}};
    snip = xd_snip_screen_cut(screen, xd_snip_box_around(triangle, 3), triangle, 3, nullptr);
    CHECK(snip != nullptr && cairo_image_surface_get_width(snip) == 40);
    CHECK(snip_pixel(snip, 4, 4) == 0x0000ff && snip_pixel(snip, 36, 36) == 0xffffff);
    cairo_surface_destroy(snip);
    xd_snip_screen_free(screen);

    /* Ink: the eraser takes whole strokes, only those it touches. */
    xd_ink ink;
    xd_ink_init(&ink);
    const double black[3] = {0, 0, 0};
    xd_ink_stroke *stroke = xd_ink_begin(&ink, XD_INK_PEN, black, 2, (xd_snip_point){0, 0});
    xd_ink_extend(stroke, (xd_snip_point){100, 0});
    xd_ink_begin(&ink, XD_INK_HIGHLIGHTER, (const double[3]){1, 1, 0}, 10, (xd_snip_point){50, 50});
    CHECK(!xd_ink_erase_at(&ink, (xd_snip_point){50, 20}, 4));
    CHECK(xd_ink_erase_at(&ink, (xd_snip_point){50, 4}, 4) && ink.strokes->len == 1);
    /* The highlighter multiplies: yellow over blue is black, over white yellow. */
    cairo_surface_t *canvas = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 100, 100);
    cairo = cairo_create(canvas);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_paint(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 1);
    cairo_rectangle(cairo, 0, 0, 50, 100);
    cairo_fill(cairo);
    xd_ink_stroke *mark =
        xd_ink_begin(&ink, XD_INK_HIGHLIGHTER, (const double[3]){1, 1, 0}, 10, (xd_snip_point){20, 80});
    xd_ink_extend(mark, (xd_snip_point){80, 80});
    xd_ink_draw(&ink, cairo);
    cairo_destroy(cairo);
    CHECK(snip_pixel(canvas, 30, 80) == 0x000000 && snip_pixel(canvas, 70, 80) == 0xffff00);
    CHECK(snip_pixel(canvas, 70, 60) == 0xffffff);
    cairo_surface_destroy(canvas);
    xd_ink_clear(&ink);
    CHECK(ink.strokes->len == 0);
    xd_ink_finish(&ink);

    /* Options: Windows 7's defaults, and back from the file. */
    xd_snip_options options;
    xd_snip_options_init(&options);
    CHECK(options.mode == XD_SNIP_RECTANGLE && options.always_copy && options.ink[0] == 1);
    options.mode = XD_SNIP_FREE_FORM;
    options.delay_seconds = 3;
    options.show_overlay = false;
    options.ink[0] = 0;
    options.ink[2] = 1;
    GKeyFile *file = g_key_file_new();
    xd_snip_options_save_data(&options, file);
    xd_snip_options loaded;
    xd_snip_options_init(&loaded);
    xd_snip_options_load_data(&loaded, file);
    CHECK(loaded.mode == XD_SNIP_FREE_FORM && loaded.delay_seconds == 3 && !loaded.show_overlay);
    CHECK(loaded.ink[0] == 0 && loaded.ink[2] == 1 && loaded.prompt_to_save);
    g_key_file_set_string(file, "Snipping Tool", "InkColor", "red");
    g_key_file_set_integer(file, "Snipping Tool", "Delay", 60);
    xd_snip_options_load_data(&loaded, file);
    CHECK(loaded.delay_seconds == 5 && loaded.ink[2] == 1);
    g_key_file_free(file);
}

/* A scripted host's session: the script written to `dir`/name, the
 * session started against it. */
static xd_hgcm *vboxdnd_host(const char *dir, const char *name, const char *script,
                             xd_dnd_session *session, char **log_path) {
    g_autofree char *path = g_build_filename(dir, name, nullptr);
    CHECK(g_file_set_contents(path, script, -1, nullptr));
    xd_hgcm *hgcm = xd_fake_host_open(path, nullptr);
    CHECK(hgcm != nullptr);
    g_autofree char *drops = g_build_filename(dir, "drops", nullptr);
    CHECK(xd_dnd_session_init(session, hgcm, drops) == XD_VINF_SUCCESS);
    *log_path = g_strconcat(path, ".log", nullptr);
    return hgcm;
}

static bool log_has(const char *path, const char *line) {
    g_autofree char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        return false;
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    return g_strv_contains((const char *const *)lines, line);
}

static void test_vboxdnd(void) {
    /* The driver's layout: a 40-byte header, then 16-byte parameters. */
    char buffer[5] = "abcd";
    xd_hgcm_parm parms[] = {XD_HGCM_PARM_U32(7), XD_HGCM_PARM_U64(UINT64_C(0x1122334455667788)),
                            XD_HGCM_PARM_BUFFER(buffer, 5)};
    unsigned char call[40 + 16 * 3];
    CHECK(xd_hgcm_encode(9, 300, parms, 3, call, sizeof(call)) == sizeof(call));
    uint32_t word;
    uint16_t count;
    uint64_t wide;
    memcpy(&word, call + 4, 4);
    CHECK(word == 0x10001); /* the header's version */
    memcpy(&word, call + 24, 4);
    CHECK(word == 9);
    memcpy(&word, call + 28, 4);
    CHECK(word == 300);
    memcpy(&count, call + 38, 2);
    CHECK(count == 3);
    memcpy(&word, call + 40, 4);
    CHECK(word == 1);
    memcpy(&word, call + 44, 4);
    CHECK(word == 7);
    memcpy(&wide, call + 60, 8);
    CHECK(wide == UINT64_C(0x1122334455667788));
    memcpy(&word, call + 72, 4);
    CHECK(word == 4);
    memcpy(&word, call + 76, 4);
    CHECK(word == 5);
    memcpy(&wide, call + 80, 8);
    CHECK(wide == (uint64_t)(uintptr_t)buffer);
    word = 42; /* the host's answer */
    memcpy(call + 44, &word, 4);
    xd_hgcm_decode(call, parms, 3);
    CHECK(parms[0].u32 == 42 && parms[2].size == 5);
    CHECK(xd_hgcm_encode(9, 300, parms, 3, call, 40) == 0); /* too small */

    /* Formats lists, names, the dropped roots. */
    static const char list[] = "text/uri-list\r\ntext/plain\r\n\r\n";
    g_auto(GStrv) formats = xd_dnd_formats_split(list, sizeof(list));
    CHECK(g_strv_length(formats) == 2 && strcmp(formats[1], "text/plain") == 0);
    g_autofree char *joined = xd_dnd_formats_join((const char *const *)formats);
    CHECK(strcmp(joined, "text/uri-list\r\ntext/plain\r\n") == 0);
    g_autofree char *empty = xd_dnd_formats_join(nullptr);
    CHECK(strcmp(empty, "\r\n") == 0); /* never empty */
    CHECK(xd_dnd_name_valid("pics/b.png") && xd_dnd_name_valid("a b.txt"));
    CHECK(!xd_dnd_name_valid("/etc/passwd") && !xd_dnd_name_valid("pics/../../x"));
    CHECK(!xd_dnd_name_valid("..") && !xd_dnd_name_valid("") && !xd_dnd_name_valid("./a"));
    static const char meta[] = "file:///a%20b.txt\r\nfile:///pics/\r\n";
    g_auto(GStrv) roots = xd_dnd_meta_roots(meta, sizeof(meta));
    CHECK(roots != nullptr && g_strv_length(roots) == 2);
    CHECK(strcmp(roots[0], "a b.txt") == 0 && strcmp(roots[1], "pics") == 0);
    static const char hostile[] = "file:///../../etc/passwd\r\n";
    CHECK(xd_dnd_meta_roots(hostile, sizeof(hostile)) == nullptr);
    g_autofree char *uris = xd_dnd_drop_uri_list("/tmp/drop", roots);
    CHECK(strcmp(uris, "file:///tmp/drop/a%20b.txt\r\nfile:///tmp/drop/pics\r\n") == 0);

    /* A whole drop from a scripted host: a directory with a file in it
     * and a file, into a fresh drop directory, offered as a URI list. */
    g_autofree char *dir = g_dir_make_tmp("xd-vboxdnd-XXXXXX", nullptr);
    CHECK(dir != nullptr);
    xd_dnd_session session;
    char *log = nullptr;
    xd_hgcm *hgcm = vboxdnd_host(dir, "drop.script",
                                 "enter 100 100\nmove 120 110\ndrop 120 110\n"
                                 "dir pics\nfile pics/b.txt Bee\nfile a.txt Hello\n",
                                 &session, &log);
    xd_dnd_event *event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_ENTER && event->x == 100 && event->y == 100);
    CHECK(event->formats != nullptr && strcmp(event->formats[0], XD_DND_URI_LIST) == 0);
    CHECK(event->allowed_actions == (XD_DND_COPY | XD_DND_MOVE));
    xd_dnd_event_free(event);
    CHECK(xd_dnd_ack(hgcm, XD_DND_COPY) == XD_VINF_SUCCESS);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_MOVE && event->x == 120);
    xd_dnd_event_free(event);
    xd_dnd_ack(hgcm, XD_DND_COPY);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DROPPED);
    xd_dnd_event_free(event);
    CHECK(xd_dnd_request_data(&session, XD_DND_URI_LIST) == XD_VINF_SUCCESS);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DATA && strcmp(event->mime, XD_DND_URI_LIST) == 0);
    g_autofree char *dropped = g_strndup(g_bytes_get_data(event->data, nullptr),
                                         g_bytes_get_size(event->data));
    xd_dnd_event_free(event);
    g_auto(GStrv) dropped_uris = g_strsplit(dropped, "\r\n", -1);
    CHECK(g_strv_length(dropped_uris) == 3 && dropped_uris[2][0] == '\0');
    g_autofree char *pics = g_filename_from_uri(dropped_uris[0], nullptr, nullptr);
    g_autofree char *a_txt = g_filename_from_uri(dropped_uris[1], nullptr, nullptr);
    CHECK(pics != nullptr && g_str_has_suffix(pics, "/pics") && a_txt != nullptr);
    g_autofree char *b_path = g_build_filename(pics, "b.txt", nullptr);
    g_autofree char *contents = nullptr;
    CHECK(g_file_get_contents(b_path, &contents, nullptr, nullptr) && strcmp(contents, "Bee") == 0);
    g_clear_pointer(&contents, g_free);
    CHECK(g_file_get_contents(a_txt, &contents, nullptr, nullptr) &&
          strcmp(contents, "Hello") == 0);
    CHECK(log_has(log, "ack 1") && log_has(log, "request text/uri-list"));
    CHECK(!log_has(log, "guest error -2"));
    xd_dnd_session_finish(&session);
    hgcm->free(hgcm);
    g_free(log);

    /* VirtualBox 7.2 may hand out the first object (and its data) before
     * the header: still one drop. */
    hgcm = vboxdnd_host(dir, "early.script", "early\ndrop 9 9\nfile a.txt Hello\nfile c.txt See\n",
                        &session, &log);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DROPPED);
    xd_dnd_event_free(event);
    xd_dnd_request_data(&session, XD_DND_URI_LIST);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DATA);
    g_autofree char *early_list = g_strndup(g_bytes_get_data(event->data, nullptr),
                                            g_bytes_get_size(event->data));
    xd_dnd_event_free(event);
    g_auto(GStrv) early_uris = g_strsplit(early_list, "\r\n", -1);
    CHECK(g_strv_length(early_uris) == 3);
    g_autofree char *early_a = g_filename_from_uri(early_uris[0], nullptr, nullptr);
    g_autofree char *early_c = g_filename_from_uri(early_uris[1], nullptr, nullptr);
    g_clear_pointer(&contents, g_free);
    CHECK(early_a != nullptr && g_file_get_contents(early_a, &contents, nullptr, nullptr) &&
          strcmp(contents, "Hello") == 0);
    g_clear_pointer(&contents, g_free);
    CHECK(early_c != nullptr && g_file_get_contents(early_c, &contents, nullptr, nullptr) &&
          strcmp(contents, "See") == 0);
    g_autofree char *early_parent_a = g_path_get_dirname(early_a);
    g_autofree char *early_parent_c = g_path_get_dirname(early_c);
    CHECK(strcmp(early_parent_a, early_parent_c) == 0);
    CHECK(!log_has(log, "guest error"));
    xd_dnd_session_finish(&session);
    hgcm->free(hgcm);
    g_free(log);

    /* A host naming a path outside the drop: refused, reported, and what
     * came of the drop removed. */
    hgcm = vboxdnd_host(dir, "hostile.script", "drop 1 1\nfile ok.txt fine\nfile ../evil x\n",
                        &session, &log);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DROPPED);
    xd_dnd_event_free(event);
    xd_dnd_request_data(&session, XD_DND_URI_LIST);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_FAILED && event->rc == XD_VERR_INVALID_PARAMETER);
    xd_dnd_event_free(event);
    CHECK(log_has(log, "guest error -2"));
    g_autofree char *evil = g_build_filename(dir, "evil", nullptr);
    CHECK(!g_file_test(evil, G_FILE_TEST_EXISTS));
    xd_dnd_session_finish(&session);
    hgcm->free(hgcm);
    g_free(log);

    /* Text, and the host's cancel and pending. */
    hgcm = vboxdnd_host(dir, "text.script",
                        "drop 5 5 text/plain;charset=utf-8\ntext Hi there\n", &session, &log);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DROPPED);
    xd_dnd_event_free(event);
    xd_dnd_request_data(&session, "text/plain;charset=utf-8");
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_DATA && g_bytes_get_size(event->data) == 8 &&
          memcmp(g_bytes_get_data(event->data, nullptr), "Hi there", 8) == 0);
    xd_dnd_event_free(event);
    xd_dnd_session_finish(&session);
    hgcm->free(hgcm);
    g_free(log);
    hgcm = vboxdnd_host(dir, "cancel.script", "pending\ncancel\n", &session, &log);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_PENDING);
    xd_dnd_event_free(event);
    CHECK(xd_dnd_ack_pending(hgcm, XD_DND_IGNORE, 0, nullptr) == XD_VINF_SUCCESS);
    CHECK(log_has(log, "pending 0 0 \\r\\n"));
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_CANCELLED);
    xd_dnd_event_free(event);
    xd_dnd_session_finish(&session);
    hgcm->free(hgcm);
    g_free(log);

    /* Guest to host: the host's drop, then a tree of files sent (a link
     * left out), and text with its NUL. */
    hgcm = vboxdnd_host(dir, "out.script", "pending\ndropped text/uri-list\n", &session, &log);
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_PENDING);
    xd_dnd_event_free(event);
    static const char *const types[] = {XD_DND_URI_LIST, "text/plain", nullptr};
    xd_dnd_ack_pending(hgcm, XD_DND_COPY, XD_DND_COPY | XD_DND_MOVE, types);
    CHECK(log_has(log, "pending 1 3 text/uri-list\\r\\ntext/plain\\r\\n"));
    event = xd_dnd_session_next(&session);
    CHECK(event->kind == XD_DND_EVENT_TAKEN && strcmp(event->mime, XD_DND_URI_LIST) == 0 &&
          event->default_action == XD_DND_COPY);
    xd_dnd_event_free(event);
    g_autofree char *out = g_build_filename(dir, "out", nullptr);
    g_autofree char *out_pics = g_build_filename(out, "pics", nullptr);
    CHECK(g_mkdir_with_parents(out_pics, 0755) == 0);
    g_autofree char *out_a = g_build_filename(out, "a.txt", nullptr);
    g_autofree char *out_b = g_build_filename(out_pics, "b.txt", nullptr);
    g_autofree char *out_link = g_build_filename(out, "link", nullptr);
    CHECK(g_file_set_contents(out_a, "A", -1, nullptr) &&
          g_file_set_contents(out_b, "Bee", -1, nullptr) && symlink(out_a, out_link) == 0);
    g_autofree char *uri_a = g_filename_to_uri(out_a, nullptr, nullptr);
    g_autofree char *uri_pics = g_filename_to_uri(out_pics, nullptr, nullptr);
    g_autofree char *uri_link = g_filename_to_uri(out_link, nullptr, nullptr);
    g_autofree char *sent = g_strdup_printf("%s\r\n%s/\r\n%s\r\n", uri_a, uri_pics, uri_link);
    CHECK(xd_dnd_send_files(hgcm, sent, strlen(sent)) == XD_VINF_SUCCESS);
    /* The meta data: the roots relative to their parent, with its NUL
     * (31 bytes); then 4 more, the files. */
    CHECK(log_has(log, "send header total 35 meta 31 format text/uri-list objects 3"));
    CHECK(log_has(log, "send data file:///a.txt\\r\\nfile:///pics/\\r\\n"));
    CHECK(log_has(log, "send file a.txt 1") && log_has(log, "send file-data A"));
    CHECK(log_has(log, "send dir pics") && log_has(log, "send file pics/b.txt 3"));
    CHECK(log_has(log, "send file-data Bee") && !log_has(log, "send file link 1"));
    CHECK(xd_dnd_send_data(hgcm, "text/plain;charset=utf-8", "Hi", 2) == XD_VINF_SUCCESS);
    CHECK(log_has(log, "send header total 3 meta 3 format  objects 0"));
    g_autofree char *log_text = nullptr;
    CHECK(g_file_get_contents(log, &log_text, nullptr, nullptr) && strstr(log_text, "error") == nullptr);
    xd_dnd_session_finish(&session);
    hgcm->free(hgcm);
    g_free(log);

    g_autofree char *remove = g_strdup_printf("rm -rf -- '%s'", dir);
    CHECK(system(remove) == 0);
}

/* A field with mines exactly where `rows` has '*' (row by row), laid
 * by hand instead of at random, already playing. */
static void minefield_from(xd_minefield *field, const char *const *rows, int width, int height) {
    int mines = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            mines += rows[y][x] == '*' ? 1 : 0;
    }
    xd_minefield_init(field, width, height, mines);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++)
            xd_minefield_cell(field, x, y)->mine = rows[y][x] == '*';
    }
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int around = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const xd_mines_cell *near = xd_minefield_cell(field, x + dx, y + dy);
                    around += near != nullptr && near->mine ? 1 : 0;
                }
            }
            xd_minefield_cell(field, x, y)->neighbors = (uint8_t)around;
        }
    }
    field->state = XD_MINES_PLAYING;
}

static void test_minefield(void) {
    int width, height, mines;
    xd_mines_level_size(XD_MINES_EXPERT, &width, &height, &mines);
    CHECK(width == 30 && height == 16 && mines == 99);
    xd_mines_level level = XD_MINES_BEGINNER;
    CHECK(xd_mines_level_parse("intermediate", &level) && level == XD_MINES_INTERMEDIATE);
    width = 50;
    height = 2;
    mines = 900;
    xd_mines_clamp_custom(&width, &height, &mines);
    CHECK(width == 30 && height == 9 && mines == 29 * 8);

    /* The first square opened is never a mine, whatever the seed; the
     * mines are all laid, and the numbers count them. */
    GRand *rand = g_rand_new_with_seed(1);
    for (int round = 0; round < 50; round++) {
        xd_minefield field;
        xd_minefield_init(&field, 9, 9, 60);
        CHECK(field.state == XD_MINES_READY);
        xd_minefield_open(&field, 4, 4, rand);
        CHECK(!xd_minefield_cell(&field, 4, 4)->mine && field.state != XD_MINES_LOST);
        int laid = 0;
        for (int i = 0; i < 81; i++)
            laid += field.cells[i].mine ? 1 : 0;
        CHECK(laid == 60);
        xd_minefield_finish(&field);
    }
    g_rand_free(rand);

    /* Beginner's 9 x 9 with its 10 mines: a wall of them down the middle
     * column, and one in the top left corner. */
    static const char *const walled[] = {
        "*...*....", "....*....", "....*....", "....*....", "....*....",
        "....*....", "....*....", "....*....", "....*....",
    };
    xd_minefield field;
    minefield_from(&field, walled, 9, 9);
    CHECK(field.mines == 10 && xd_minefield_cell(&field, 1, 1)->neighbors == 1);
    /* An empty square opens its whole area, to the numbers at its edge:
     * the right of the wall, not the left. */
    CHECK(xd_minefield_open(&field, 8, 0, nullptr) == 36 && field.state == XD_MINES_PLAYING);
    CHECK(xd_minefield_cell(&field, 5, 4)->mark == XD_MINES_OPEN);
    CHECK(xd_minefield_cell(&field, 1, 1)->mark == XD_MINES_COVERED);
    /* Flags: counted, kept from opening, ? after with marks on. */
    xd_minefield_cycle_mark(&field, 0, 0, true);
    CHECK(xd_minefield_cell(&field, 0, 0)->mark == XD_MINES_FLAGGED &&
          xd_minefield_mines_left(&field) == 9);
    CHECK(xd_minefield_open(&field, 0, 0, nullptr) == 0);
    xd_minefield_cycle_mark(&field, 0, 0, true);
    CHECK(xd_minefield_cell(&field, 0, 0)->mark == XD_MINES_QUESTIONED &&
          xd_minefield_mines_left(&field) == 10);
    xd_minefield_cycle_mark(&field, 0, 0, true);
    CHECK(xd_minefield_cell(&field, 0, 0)->mark == XD_MINES_COVERED);
    xd_minefield_cycle_mark(&field, 0, 0, false);
    xd_minefield_cycle_mark(&field, 0, 0, false); /* no ? without marks */
    CHECK(xd_minefield_cell(&field, 0, 0)->mark == XD_MINES_COVERED);

    /* Chording: the 1 at (1,1), its mine flagged, opens the rest around
     * it; not before the flag is there. */
    CHECK(xd_minefield_open(&field, 1, 1, nullptr) == 1);
    CHECK(xd_minefield_chord(&field, 1, 1) == 0);
    xd_minefield_cycle_mark(&field, 0, 0, false);
    /* Its empty neighbour opens the rest of the left side, the last of
     * the safe squares: that wins, and flags every mine. */
    CHECK(xd_minefield_chord(&field, 1, 1) > 0);
    CHECK(xd_minefield_cell(&field, 2, 2)->mark == XD_MINES_OPEN);
    CHECK(field.state == XD_MINES_WON && xd_minefield_mines_left(&field) == 0);
    CHECK(xd_minefield_cell(&field, 4, 4)->mark == XD_MINES_FLAGGED);
    xd_minefield_finish(&field);

    /* A wrong flag, chorded: the mine it should have been on goes off. */
    minefield_from(&field, walled, 9, 9);
    xd_minefield_open(&field, 1, 1, nullptr);
    xd_minefield_cycle_mark(&field, 1, 0, false);
    CHECK(xd_minefield_chord(&field, 1, 1) > 0);
    CHECK(field.state == XD_MINES_LOST && field.exploded == 0);
    CHECK(xd_minefield_open(&field, 2, 2, nullptr) == 0); /* over: nothing more */
    xd_minefield_finish(&field);
}

static void test_recording(void) {
    GDateTime *time = g_date_time_new_local(2026, 10, 22, 15, 37, 4);
    char *name = xd_record_file_name(time);
    CHECK(strcmp(name, "Screen Recording 2026-10-22 153704.mp4") == 0);
    g_free(name);
    g_date_time_unref(time);
    char *folder = xd_record_folder("/home/ann/Movies", "/home/ann");
    CHECK(strcmp(folder, "/home/ann/Movies/Screen Recordings") == 0);
    g_free(folder);
    folder = xd_record_folder("/home/ann", "/home/ann"); /* user-dirs.dirs names none */
    CHECK(strcmp(folder, "/home/ann/Videos/Screen Recordings") == 0);
    g_free(folder);

    /* Within the monitor, whole pixels; nothing outside it. */
    xd_snip_box monitor = {1920, 0, 1280, 1024}, area;
    CHECK(xd_record_fit((xd_snip_box){1800, 100.4, 300, 200.2}, monitor, &area));
    CHECK(area.x == 1920 && area.y == 100 && area.width == 180 && area.height == 201);
    CHECK(!xd_record_fit((xd_snip_box){0, 0, 1900, 1000}, monitor, &area));

    CHECK(strcmp(xd_record_video_format(WL_SHM_FORMAT_XRGB8888), "BGRx") == 0);
    CHECK(strcmp(xd_record_video_format(WL_SHM_FORMAT_ABGR8888), "RGBA") == 0);
    CHECK(xd_record_video_format(WL_SHM_FORMAT_RGB565) == nullptr);

    xd_record_sound sound = XD_RECORD_SILENT;
    CHECK(xd_record_sound_parse("both", &sound) && sound == XD_RECORD_MICROPHONE_AND_COMPUTER);
    CHECK(!xd_record_sound_parse("loud", &sound) && strcmp(xd_record_sound_name(sound), "both") == 0);

    /* Silent: no sound source at all, even with an AAC encoder. */
    char *pipeline = xd_record_pipeline("x264enc", "avenc_aac", XD_RECORD_SILENT);
    CHECK(strstr(pipeline, "appsrc name=video") != nullptr && strstr(pipeline, "! x264enc !") != nullptr);
    CHECK(strstr(pipeline, "mp4mux name=mux") != nullptr && strstr(pipeline, "filesink name=file") != nullptr);
    CHECK(strstr(pipeline, "pulsesrc") == nullptr);
    g_free(pipeline);
    pipeline = xd_record_pipeline("x264enc", "avenc_aac", XD_RECORD_COMPUTER);
    CHECK(strstr(pipeline, "pulsesrc device=@DEFAULT_MONITOR@") != nullptr &&
          strstr(pipeline, "avenc_aac ! queue ! mux.") != nullptr);
    g_free(pipeline);
    pipeline = xd_record_pipeline("x264enc", "fdkaacenc", XD_RECORD_MICROPHONE_AND_COMPUTER);
    CHECK(strstr(pipeline, "audiomixer name=mix") != nullptr);
    CHECK(strstr(pipeline, "pulsesrc ! audioconvert") != nullptr &&
          strstr(pipeline, "pulsesrc device=@DEFAULT_MONITOR@") != nullptr);
    g_free(pipeline);
    /* No encoder for the sound: silent. */
    pipeline = xd_record_pipeline("openh264enc", nullptr, XD_RECORD_MICROPHONE);
    CHECK(strstr(pipeline, "pulsesrc") == nullptr);
    g_free(pipeline);
}

static void test_notices(void) {
    char *plain = xd_notice_plain_text("<b>Bold</b> &amp; <a href='https://x'>link</a>"
                                       "<img src='a.png' alt=' [pic]'/>");
    CHECK(g_strcmp0(plain, "Bold & link [pic]") == 0);
    g_free(plain);
    plain = xd_notice_plain_text("Tom & Jerry <3");
    CHECK(g_strcmp0(plain, "Tom & Jerry <3") == 0); /* not markup: as it is */
    g_free(plain);

    /* A 2x1 RGBA picture: opaque red, half-transparent white. */
    static const uint8_t pixels[] = {255, 0, 0, 255, 255, 255, 255, 128};
    GVariant *image = g_variant_new("(iiibii@ay)", 2, 1, 8, TRUE, 8, 4,
                                    g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixels,
                                                              sizeof(pixels), 1));
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(2));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("org.example.Mail"));
    g_variant_builder_add(&hints, "{sv}", "image-data", image);
    GVariant *call = notify_call("New mail", "From <i>Ann</i>", 0, g_variant_builder_end(&hints));
    uint32_t replaces = 99;
    xd_notice *notice = xd_notice_parse(call, ":1.5", &replaces, nullptr);
    g_variant_unref(call);
    CHECK(notice != nullptr && replaces == 0);
    CHECK(g_strcmp0(notice->summary, "New mail") == 0 && g_strcmp0(notice->body, "From Ann") == 0);
    CHECK(notice->urgency == XD_NOTICE_CRITICAL && g_strcmp0(notice->icon, "mail-unread") == 0);
    CHECK(g_strcmp0(notice->desktop_entry, "org.example.Mail.desktop") == 0);
    /* "default" is a click on it; the dangling "odd" is dropped. */
    CHECK(xd_notice_button_count(notice) == 1 &&
          g_strcmp0(xd_notice_button_key(notice, 0), "reply") == 0);
    CHECK(g_strcmp0(xd_notice_action_label(notice, "default"), "Open") == 0);
    CHECK(notice->image != nullptr && cairo_image_surface_get_width(notice->image) == 2);
    if (notice->image != nullptr) {
        const uint32_t *argb = (const uint32_t *)(const void *)cairo_image_surface_get_data(
            notice->image);
        CHECK(argb[0] == 0xFFFF0000u && argb[1] == 0x80808080u); /* premultiplied */
    }

    /* Nonsense: the wrong signature, a picture whose rows don't fit. */
    GVariant *wrong = g_variant_ref_sink(g_variant_new("(s)", "x"));
    GError *error = nullptr;
    CHECK(xd_notice_parse(wrong, ":1.5", &replaces, &error) == nullptr && error != nullptr);
    g_clear_error(&error);
    g_variant_unref(wrong);
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "image-data",
                          g_variant_new("(iiibii@ay)", 64, 64, 256, TRUE, 8, 4,
                                        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixels,
                                                                  sizeof(pixels), 1)));
    call = notify_call("Short picture", "", 0, g_variant_builder_end(&hints));
    xd_notice *short_picture = xd_notice_parse(call, ":1.5", &replaces, nullptr);
    g_variant_unref(call);
    CHECK(short_picture != nullptr && short_picture->image == nullptr);

    xd_notices store;
    xd_notices_init(&store);
    bool replaced = true;
    xd_notice *kept = xd_notices_add(&store, notice, 0, &replaced);
    CHECK(!replaced && kept->id == 1);
    xd_notices_add(&store, short_picture, 0, &replaced);
    CHECK(short_picture->id == 2);
    /* Its own program replaces it, in place and newest; another can't. */
    call = notify_call("Two new mails", "", 1, g_variant_new("a{sv}", nullptr));
    xd_notice *update = xd_notice_parse(call, ":1.5", &replaces, nullptr);
    g_variant_unref(call);
    CHECK(update != nullptr && replaces == 1);
    xd_notices_add(&store, update, replaces, &replaced);
    CHECK(replaced && update->id == 1 && store.list->len == 2);
    CHECK(g_ptr_array_index(store.list, 1) == update);
    call = notify_call("Not yours", "", 1, g_variant_new("a{sv}", nullptr));
    xd_notice *other = xd_notice_parse(call, ":1.9", &replaces, nullptr);
    g_variant_unref(call);
    xd_notices_add(&store, other, replaces, &replaced);
    CHECK(!replaced && other->id == 3 && store.list->len == 3);
    other->transient = true;
    update->unread = true;
    CHECK(xd_notices_unread(&store) == 1);

    /* The history's file: not the transient one; the rest as closed. */
    char *data = xd_notices_save_data(&store);
    xd_notices loaded;
    xd_notices_init(&loaded);
    xd_notices_load_data(&loaded, data);
    g_free(data);
    CHECK(loaded.list->len == 2);
    const xd_notice *first = g_ptr_array_index(loaded.list, 1);
    CHECK(g_strcmp0(first->summary, "Two new mails") == 0 && first->unread && first->closed);
    CHECK(first->sender == nullptr && first->actions[0] == nullptr && first->image == nullptr);
    xd_notices_load_data(&loaded, "not a key file [");
    CHECK(loaded.list->len == 2);
    xd_notices_finish(&loaded);

    /* The oldest go past the limit, but not the one kept back. */
    GArray *gone = xd_notices_trim(&store, 1, 2);
    CHECK(gone->len == 2 && store.list->len == 1 && xd_notices_find(&store, 2) != nullptr);
    g_array_unref(gone);
    CHECK(xd_notices_remove(&store, 2) && !xd_notices_remove(&store, 2));
    xd_notices_finish(&store);
}

static void test_calendar(void) {
    CHECK(xd_calendar_weekday((xd_day){2026, 10, 21}) == 3); /* a Wednesday */
    CHECK(xd_calendar_weekday((xd_day){2026, 2, 1}) == 0); /* a Sunday */
    xd_day page[XD_CALENDAR_CELLS];
    /* October 2026 starts on a Thursday: Sunday weeks start Sep 27. */
    xd_calendar_page(2026, 10, 0, page);
    CHECK(xd_calendar_same(page[0], (xd_day){2026, 9, 27}));
    CHECK(xd_calendar_same(page[4], (xd_day){2026, 10, 1}));
    CHECK(xd_calendar_same(page[XD_CALENDAR_CELLS - 1], (xd_day){2026, 11, 7}));
    /* Monday weeks start Sep 28. */
    xd_calendar_page(2026, 10, 1, page);
    CHECK(xd_calendar_same(page[0], (xd_day){2026, 9, 28}));
    CHECK(xd_calendar_weekday(page[0]) == 1);
    /* February 2026 starts on a Sunday: a whole week of January first. */
    xd_calendar_page(2026, 2, 0, page);
    CHECK(xd_calendar_same(page[0], (xd_day){2026, 1, 25}));
    CHECK(xd_calendar_same(page[7], (xd_day){2026, 2, 1}));
    CHECK(xd_calendar_same(xd_calendar_add_days((xd_day){2026, 12, 31}, 1), (xd_day){2027, 1, 1}));
    CHECK(xd_calendar_same(xd_calendar_add_days((xd_day){2026, 3, 1}, -1), (xd_day){2026, 2, 28}));
    CHECK(
        xd_calendar_same(xd_calendar_add_months((xd_day){2026, 1, 31}, 1), (xd_day){2026, 2, 28}));
    CHECK(xd_calendar_same(xd_calendar_add_months((xd_day){2026, 1, 15}, -1),
                           (xd_day){2025, 12, 15}));
    int first = xd_calendar_first_weekday();
    CHECK(first >= 0 && first <= 6);
}

static void test_superbar(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(original.buttons == XD_BUTTONS_ALWAYS_COMBINE); /* Windows 7's default */
    xd_settings_parse(XD_STR("[Taskbar]\nButtons=whenfull\n"), &original);
    CHECK(original.buttons == XD_BUTTONS_WHEN_FULL);
    xd_strbuf text = {0};
    xd_settings_serialize(&original, &text);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(parsed.buttons == XD_BUTTONS_WHEN_FULL && xd_settings_equal(&original, &parsed));
    xd_strbuf_free(&text);
    xd_settings_parse(XD_STR("[Taskbar]\nButtons=sometimes\n"), &original);
    CHECK(original.buttons == XD_BUTTONS_ALWAYS_COMBINE);

    /* The unified taskbar and its icons: Aero's by default, any style's
     * when chosen; large icons go with it unless small ones are chosen. */
    xd_settings layout;
    xd_settings_defaults(&layout);
    CHECK(layout.layout == XD_TASKBAR_LAYOUT_AUTO && layout.icons == XD_TASKBAR_ICONS_AUTO);
    CHECK(xd_settings_taskbar_unified(&layout, XD_STYLE_AERO));
    CHECK(!xd_settings_taskbar_unified(&layout, XD_STYLE_LUNA));
    CHECK(xd_settings_taskbar_large_icons(&layout, XD_STYLE_AERO));
    CHECK(!xd_settings_taskbar_large_icons(&layout, XD_STYLE_CLASSIC));
    xd_settings_parse(XD_STR("[Taskbar]\nLayout=unified\nIcons=small\n"), &layout);
    CHECK(xd_settings_taskbar_unified(&layout, XD_STYLE_LUNA));
    CHECK(!xd_settings_taskbar_large_icons(&layout, XD_STYLE_LUNA));
    xd_settings_serialize(&layout, &text);
    xd_settings_parse(xd_strbuf_view(&text), &parsed);
    CHECK(parsed.layout == XD_TASKBAR_LAYOUT_UNIFIED && parsed.icons == XD_TASKBAR_ICONS_SMALL);
    xd_strbuf_free(&text);
    xd_settings_parse(XD_STR("[Taskbar]\nLayout=tasks\nIcons=large\n"), &layout);
    CHECK(!xd_settings_taskbar_unified(&layout, XD_STYLE_AERO));
    CHECK(xd_settings_taskbar_large_icons(&layout, XD_STYLE_AERO));

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
        char *size_text = xd_print_size_text(sizes[i].bytes);
        CHECK(strcmp(size_text, sizes[i].text) == 0);
        if (strcmp(size_text, sizes[i].text) != 0)
            fprintf(stderr, "  size %" G_GUINT64_FORMAT ": %s\n", sizes[i].bytes, size_text);
        g_free(size_text);
    }
    char *pages_text = xd_print_pages_text(0, 0, false);
    CHECK(strcmp(pages_text, "N/A") == 0);
    g_free(pages_text);
    pages_text = xd_print_pages_text(2, 5, true);
    CHECK(strcmp(pages_text, "2/5") == 0);
    g_free(pages_text);
    pages_text = xd_print_pages_text(0, 5, false);
    CHECK(strcmp(pages_text, "5") == 0);
    g_free(pages_text);

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
        char *port_text = xd_printer_port_text(ports[i][0]);
        CHECK(strcmp(port_text, ports[i][1]) == 0);
        if (strcmp(port_text, ports[i][1]) != 0)
            fprintf(stderr, "  port %s: %s\n", ports[i][0], port_text);
        g_free(port_text);
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
    char *name = xd_printer_name_suggest("HP LaserJet 4 (Room #2)");
    CHECK(strcmp(name, "HP_LaserJet_4_(Room_2)") == 0);
    CHECK(xd_printer_name_valid(name));
    g_free(name);
    name = xd_printer_name_suggest("  ");
    CHECK(strcmp(name, "Printer") == 0);
    g_free(name);
    char long_name[300];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    name = xd_printer_name_suggest(long_name);
    CHECK(strlen(name) == 127 && xd_printer_name_valid(name));
    g_free(name);

    CHECK(strcmp(xd_print_orientation_label(4), "Landscape") == 0);
    CHECK(xd_print_orientation_label(7) == nullptr);
    CHECK(strcmp(xd_print_sides_label("two-sided-long-edge"), "Flip on Long Edge") == 0);
    CHECK(strcmp(xd_print_color_label("monochrome"), "Black & White") == 0);
    CHECK(strcmp(xd_print_quality_label(5), "Best") == 0);
    char *label = xd_print_keyword_label("stationery-heavyweight");
    CHECK(strcmp(label, "Stationery heavyweight") == 0);
    g_free(label);

    /* The Ports tab's typed URIs, and Add Port's start for a kind of port. */
    CHECK(xd_printer_device_uri_complete("socket://192.168.1.5:9100"));
    CHECK(xd_printer_device_uri_complete("usb://HP/LaserJet?serial=1"));
    CHECK(xd_printer_device_uri_complete("file:/dev/null"));
    CHECK(xd_printer_device_uri_complete("parallel:/dev/lp0"));
    CHECK(!xd_printer_device_uri_complete("socket"));
    CHECK(!xd_printer_device_uri_complete("socket://"));
    CHECK(!xd_printer_device_uri_complete("ipp://"));
    CHECK(!xd_printer_device_uri_complete(""));
    CHECK(!xd_printer_device_uri_complete(nullptr));
    label = xd_printer_device_uri_start("socket");
    CHECK(strcmp(label, "socket://") == 0);
    g_free(label);
    label = xd_printer_device_uri_start("usb://HP/x");
    CHECK(strcmp(label, "usb://HP/x") == 0);
    g_free(label);

    /* Properties' Apply: one CUPS-Add-Modify-Printer with what changed. */
    xd_printer_changes printer_changes = {
        .info = "Hall", .device_uri = "socket://10.0.0.9", .shared = 1};
    ipp_t *req = xd_printer_modify_request("Office_Laser", &printer_changes);
    CHECK(ippGetOperation(req) == IPP_OP_CUPS_ADD_MODIFY_PRINTER);
    ipp_attribute_t *ipp_attribute = ippFindAttribute(req, "printer-uri", IPP_TAG_URI);
    CHECK(ipp_attribute != nullptr &&
          g_str_has_suffix(ippGetString(ipp_attribute, 0, nullptr), "/printers/Office_Laser"));
    ipp_attribute = ippFindAttribute(req, "device-uri", IPP_TAG_URI);
    CHECK(ipp_attribute != nullptr && ippGetGroupTag(ipp_attribute) == IPP_TAG_PRINTER &&
          strcmp(ippGetString(ipp_attribute, 0, nullptr), "socket://10.0.0.9") == 0);
    ipp_attribute = ippFindAttribute(req, "printer-is-shared", IPP_TAG_BOOLEAN);
    CHECK(ipp_attribute != nullptr && ippGetGroupTag(ipp_attribute) == IPP_TAG_PRINTER &&
          ippGetBoolean(ipp_attribute, 0));
    ipp_attribute = ippFindAttribute(req, "printer-info", IPP_TAG_TEXT);
    CHECK(ipp_attribute != nullptr && strcmp(ippGetString(ipp_attribute, 0, nullptr), "Hall") == 0);
    CHECK(ippFindAttribute(req, "printer-location", IPP_TAG_ZERO) == nullptr);
    ippDelete(req);
    /* Unchanged port and sharing: neither is sent. */
    printer_changes = (xd_printer_changes){.info = "", .location = "", .shared = -1};
    req = xd_printer_modify_request("Office_Laser", &printer_changes);
    CHECK(ippFindAttribute(req, "device-uri", IPP_TAG_ZERO) == nullptr);
    CHECK(ippFindAttribute(req, "printer-is-shared", IPP_TAG_ZERO) == nullptr);
    printer_changes.shared = 0;
    ippDelete(req);
    req = xd_printer_modify_request("Office_Laser", &printer_changes);
    ipp_attribute = ippFindAttribute(req, "printer-is-shared", IPP_TAG_BOOLEAN);
    CHECK(ipp_attribute != nullptr && !ippGetBoolean(ipp_attribute, 0));
    ippDelete(req);
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

    GArray *array = g_array_new(FALSE, FALSE, sizeof(int));
    xd_scan_resolutions_in_range(1, 1200, array);
    CHECK(array->len == 7 && g_array_index(array, int, 0) == 75 &&
          g_array_index(array, int, 6) == 1200);
    g_array_set_size(array, 0);
    xd_scan_resolutions_in_range(110, 140, array);
    CHECK(array->len == 2 && g_array_index(array, int, 0) == 110 &&
          g_array_index(array, int, 1) == 140);
    g_array_unref(array);
    CHECK(xd_scan_preview_dpi(1, 1200) == 50);
    CHECK(xd_scan_preview_dpi(75, 600) == 75);

    CHECK(xd_scan_name_valid("Picture"));
    CHECK(!xd_scan_name_valid("a/b") && !xd_scan_name_valid("..") && !xd_scan_name_valid(""));
    char *dir = g_dir_make_tmp("xd-scan-XXXXXX", nullptr);
    char *file_path = xd_scan_next_file(dir, "Picture", "png");
    char *want = g_build_filename(dir, "Picture 001.png", nullptr);
    CHECK(strcmp(file_path, want) == 0);
    CHECK(g_file_set_contents(file_path, "", 0, nullptr));
    char *f2 = xd_scan_next_file(dir, "Picture", "png");
    CHECK(g_str_has_suffix(f2, "Picture 002.png"));
    CHECK(g_remove(file_path) == 0);
    CHECK(g_rmdir(dir) == 0);
    g_free(f2);
    g_free(want);
    g_free(file_path);
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
        const xd_netif *netif = (const xd_netif *)(const void *)ifs->data;
        CHECK(strcmp(netif[0].name, "br0") == 0 && netif[0].state == XD_NETIF_NON_OPERATIONAL);
        CHECK(strcmp(netif[1].name, "eth0") == 0 && netif[1].state == XD_NETIF_OPERATIONAL &&
              netif[1].speed_mbps == 1000 && netif[1].rx_bytes == 1000000 &&
              netif[1].tx_bytes == 250000);
        CHECK(strcmp(netif[2].name, "eth1") == 0 && netif[2].state == XD_NETIF_DISCONNECTED &&
              netif[2].speed_mbps == 0);
        CHECK(strcmp(netif[3].name, "vpn0") == 0 && netif[3].state == XD_NETIF_OPERATIONAL);
        CHECK(strcmp(netif[4].name, "wlan0") == 0 && netif[4].speed_mbps == 0);
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
        char *speed_text = xd_netif_speed_text(speeds[i].mbps);
        CHECK(strcmp(speed_text, speeds[i].text) == 0);
        g_free(speed_text);
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
        char *utilization_text = xd_netif_utilization_text(utils[i].share);
        CHECK(strcmp(utilization_text, utils[i].text) == 0);
        g_free(utilization_text);
    }

    CHECK(xd_status_peak_kb("Name:\tbash\nVmPeak:\t  20000 kB\nVmHWM:\t    5120 kB\n"
                            "VmRSS:\t    4096 kB\n") == 5120);
    CHECK(xd_status_peak_kb("Name:\tkthreadd\nThreads:\t1\n") == 0);
    CHECK(xd_status_peak_kb("") == 0);
    /* Our own: at least what we use now. */
    CHECK(xd_proc_peak_kb((int)getpid()) > 0);
    CHECK(xd_proc_peak_kb(-1) == 0);
}

static const xd_autostart *autostart_at(GPtrArray *list, const char *file) {
    for (guint i = 0; i < list->len; i++) {
        const xd_autostart *autostart = g_ptr_array_index(list, i);
        if (strcmp(autostart->file, file) == 0)
            return autostart;
    }
    return nullptr;
}

static bool file_has(const char *path, const char *text) {
    char *data = nullptr;
    bool has = g_file_get_contents(path, &data, nullptr, nullptr) && strstr(data, text) != nullptr;
    g_free(data);
    return has;
}

static void test_startup_programs(void) {
    char *root = g_dir_make_tmp("xd-autostart-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    const char *app = "[Desktop Entry]\nType=Application\nExec=true\n";
    char *contents;
#define PUT(rel, extra)                                                                            \
    do {                                                                                           \
        contents = g_strconcat(app, extra, nullptr);                                               \
        put_file(root, rel, contents);                                                             \
        g_free(contents);                                                                          \
    } while (0)
    PUT("sys1/a.desktop", "Name=Alpha\nIcon=alpha\n");
    PUT("sys2/a.desktop", "Name=Shadowed\n"); /* sys1's comes first */
    PUT("sys1/gnome.desktop", "Name=Gnome Only\nOnlyShowIn=GNOME;\n");
    PUT("sys1/notus.desktop", "Name=Not Us\nNotShowIn=XeonDesktop;\n");
    PUT("sys1/us.desktop", "Name=Us\nOnlyShowIn=KDE;XeonDesktop;\n");
    PUT("sys1/skip.desktop", "Name=Skip\nX-systemd-skip=true\n");
    PUT("sys1/phase.desktop", "Name=Phase\nX-GNOME-Autostart-Phase=Initialization\n");
    PUT("sys1/notry.desktop", "Name=No Try\nTryExec=/nonexistent/xd-no-such\n");
    PUT("sys1/off.desktop", "Name=Off\nX-GNOME-Autostart-enabled=false\n");
    PUT("sys2/hid.desktop", "Name=Hid\n");
    PUT("user/hid.desktop", "Name=Hid\nHidden=true\n");
    PUT("user/mine.desktop", "Name=Mine\n");
    PUT("sys1/gone.desktop", "Name=Gone\nOnlyShowIn=GNOME;\n");
    PUT("user/gone.desktop", "Name=Gone\nHidden=true\n"); /* hides one not shown here */
#undef PUT
    char *user = g_build_filename(root, "user", nullptr);
    char *sys1 = g_build_filename(root, "sys1", nullptr);
    char *sys2 = g_build_filename(root, "sys2", nullptr);
    const char *sys[] = {sys1, sys2, nullptr};

    GPtrArray *list = xd_autostart_list(user, sys, "XeonDesktop");
    CHECK(list->len == 5); /* Alpha, Hid, Mine, Off, Us: by name */
    const char *order[] = {"Alpha", "Hid", "Mine", "Off", "Us"};
    for (guint i = 0; i < list->len && i < G_N_ELEMENTS(order); i++)
        CHECK(strcmp(((const xd_autostart *)g_ptr_array_index(list, i))->name, order[i]) == 0);
    const xd_autostart *autostart = autostart_at(list, "a.desktop");
    CHECK(autostart != nullptr && autostart->enabled && !autostart->user_only &&
          strcmp(autostart->icon, "alpha") == 0 && strcmp(autostart->location, sys1) == 0 &&
          strcmp(autostart->command, "true") == 0);
    const xd_autostart *hid = autostart_at(list, "hid.desktop");
    CHECK(hid != nullptr && !hid->enabled && !hid->user_only && strcmp(hid->location, sys2) == 0);
    const xd_autostart *mine = autostart_at(list, "mine.desktop");
    CHECK(mine != nullptr && mine->enabled && mine->user_only &&
          strcmp(mine->location, user) == 0);
    const xd_autostart *off = autostart_at(list, "off.desktop");
    CHECK(off != nullptr && !off->enabled);

    /* Off: your copy with Hidden=true; on again: the copy goes. */
    GError *err = nullptr;
    char *ua = g_build_filename(user, "a.desktop", nullptr);
    CHECK(autostart != nullptr && xd_autostart_set_enabled(autostart, user, false, &err));
    CHECK(file_has(ua, "Hidden=true") && file_has(ua, "Name=Alpha"));
    CHECK(autostart != nullptr && xd_autostart_set_enabled(autostart, user, true, &err));
    CHECK(!g_file_test(ua, G_FILE_TEST_EXISTS));
    /* The hiding copy of Hid goes too; GNOME's switch is turned on in a copy. */
    char *uh = g_build_filename(user, "hid.desktop", nullptr);
    CHECK(hid != nullptr && xd_autostart_set_enabled(hid, user, true, &err));
    CHECK(!g_file_test(uh, G_FILE_TEST_EXISTS));
    char *uo = g_build_filename(user, "off.desktop", nullptr);
    CHECK(off != nullptr && xd_autostart_set_enabled(off, user, true, &err));
    CHECK(file_has(uo, "X-GNOME-Autostart-enabled=true") && !file_has(uo, "Hidden"));
    /* Your own: Hidden in place, and Remove deletes it. */
    char *um = g_build_filename(user, "mine.desktop", nullptr);
    CHECK(mine != nullptr && xd_autostart_set_enabled(mine, user, false, &err));
    CHECK(file_has(um, "Hidden=true"));
    CHECK(err == nullptr);
    g_ptr_array_unref(list);

    list = xd_autostart_list(user, sys, "XeonDesktop");
    CHECK(list->len == 5);
    CHECK(autostart_at(list, "a.desktop") && autostart_at(list, "a.desktop")->enabled);
    CHECK(autostart_at(list, "hid.desktop") && autostart_at(list, "hid.desktop")->enabled);
    CHECK(autostart_at(list, "off.desktop") && autostart_at(list, "off.desktop")->enabled);
    mine = autostart_at(list, "mine.desktop");
    CHECK(mine != nullptr && !mine->enabled && mine->user_only);
    CHECK(mine != nullptr && xd_autostart_remove(mine, &err) && !g_file_test(um, G_FILE_TEST_EXISTS));
    /* A system entry can't be removed, only turned off. */
    CHECK(!xd_autostart_remove(autostart_at(list, "a.desktop"), &err) && err != nullptr);
    g_clear_error(&err);
    g_ptr_array_unref(list);
    /* Another desktop sees its own. */
    list = xd_autostart_list(user, sys, "GNOME");
    CHECK(autostart_at(list, "gnome.desktop") != nullptr && autostart_at(list, "us.desktop") == nullptr &&
          autostart_at(list, "notus.desktop") != nullptr);
    g_ptr_array_unref(list);

    /* Adding: a command (quoted for Exec, never over another entry), a program. */
    const char *argv[] = {"/usr/bin/foo", "a b", "$HOME", "50%", "plain", nullptr};
    char *exec = xd_exec_quote(argv);
    CHECK(strcmp(exec, "/usr/bin/foo \"a b\" \"\\$HOME\" 50%% plain") == 0);
    g_free(exec);
    char *f1 = xd_autostart_add_command(user, "true --flag 'a b' 100%", &err);
    char *f2 = xd_autostart_add_command(user, "true", &err);
    CHECK(f1 != nullptr && strcmp(f1, "true.desktop") == 0);
    CHECK(f2 != nullptr && strcmp(f2, "true-2.desktop") == 0);
    CHECK(xd_autostart_add_command(user, "broken 'quote", &err) == nullptr && err != nullptr);
    g_clear_error(&err);
    list = xd_autostart_list(user, sys, "XeonDesktop");
    const xd_autostart *tool = autostart_at(list, "true.desktop");
    CHECK(tool != nullptr && tool->enabled && tool->user_only &&
          strcmp(tool->name, "true") == 0 &&
          strcmp(tool->command, "true --flag \"a b\" 100%%") == 0);
    g_ptr_array_unref(list);
    put_file(root, "apps/org.example.Game.desktop",
             "[Desktop Entry]\nType=Application\nName=Game\nExec=true --play\nNoDisplay=true\n");
    char *src = g_build_filename(root, "apps", "org.example.Game.desktop", nullptr);
    GDesktopAppInfo *info = g_desktop_app_info_new_from_filename(src);
    CHECK(info != nullptr);
    if (info != nullptr) {
        char *f3 = xd_autostart_add_app(user, info, &err);
        CHECK(f3 != nullptr && strcmp(f3, "org.example.Game.desktop") == 0);
        char *u3 = g_build_filename(user, f3 != nullptr ? f3 : "x", nullptr);
        CHECK(file_has(u3, "Exec=true --play"));
        g_free(u3);
        g_free(f3);
        g_object_unref(info);
    }
    CHECK(err == nullptr);
    g_free(src);
    g_free(f1);
    g_free(f2);
    g_free(ua);
    g_free(uh);
    g_free(uo);
    g_free(um);
    g_free(user);
    g_free(sys1);
    g_free(sys2);
    rm_tree(root);
    g_free(root);
}

static void test_startup_services(void) {
    const char *home = "/home/u/.config/systemd/user";
    bool global = true;
    const char *mine[] = {"/home/u/.config/systemd/user/default.target.wants/a.service", nullptr};
    CHECK(xd_service_links_start_session(mine, home, &global) && !global);
    const char *gs[] = {"/home/u/.config/systemd/user/sockets.target.wants/a.service",
                        "/home/u/.config/systemd/user/graphical-session.target.wants/a.service",
                        nullptr};
    CHECK(xd_service_links_start_session(gs, home, &global) && !global);
    const char *all[] = {"/etc/systemd/user/default.target.wants/p.service", nullptr};
    CHECK(xd_service_links_start_session(all, home, &global) && global);
    const char *other[] = {"/home/u/.config/systemd/user/timers.target.wants/t.service", nullptr};
    CHECK(!xd_service_links_start_session(other, home, &global) && !global);
    CHECK(!xd_service_links_start_session(nullptr, home, &global));

    /* The services turned off here, remembered. */
    char *root = g_dir_make_tmp("xd-startup-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    char *path = g_build_filename(root, "xeondesktop", "startup.ini", nullptr);
    char **none = xd_startup_disabled_load(path);
    CHECK(none != nullptr && none[0] == nullptr);
    g_strfreev(none);
    const char *units[] = {"a.service", "b.service", nullptr};
    GError *err = nullptr;
    CHECK(xd_startup_disabled_save(path, units, &err));
    char **back = xd_startup_disabled_load(path);
    CHECK(back != nullptr && g_strv_length(back) == 2 && strcmp(back[1], "b.service") == 0);
    g_strfreev(back);
    const char *empty[] = {nullptr};
    CHECK(xd_startup_disabled_save(path, empty, &err) && !file_has(path, "Disabled"));
    CHECK(err == nullptr);
    g_free(path);
    rm_tree(root);
    g_free(root);

    /* Run's Windows names. */
    char *id = xd_run_app_path(XD_STR("xd-no-such-windows-name"));
    CHECK(id == nullptr);
    CHECK(xd_run_app_path(XD_STR("two words")) == nullptr);
    CHECK(xd_run_app_path(XD_STR("/usr/bin/msconfig")) == nullptr);
}

static void test_gpu(void) {
    GVariant *variant = g_variant_ref_sink(g_variant_new_parsed(
        "[{'Name': <'Intel UHD'>, 'Environment': <['DRI_PRIME', '0']>, 'Default': <true>},"
        " {'Name': <'NVIDIA'>, 'Environment': <['DRI_PRIME', '1', 'ODD']>, 'Default': <false>},"
        " {'Default': <false>}]"));
    xd_gpu_list gpus;
    xd_gpu_list_parse(variant, &gpus);
    g_variant_unref(variant);
    CHECK(gpus.count == 3);
    CHECK(xd_gpu_list_default(&gpus) == 0 && xd_gpu_list_other(&gpus) == 1);
    if (gpus.count == 3) {
        CHECK(strcmp(gpus.items[1].name, "NVIDIA") == 0 && !gpus.items[1].is_default);
        /* Pairs only: the odd one out is dropped. */
        CHECK(g_strv_length(gpus.items[1].env) == 2 &&
              strcmp(gpus.items[1].env[1], "1") == 0);
        CHECK(gpus.items[2].env != nullptr && gpus.items[2].env[0] == nullptr &&
              gpus.items[2].name[0] != '\0');
    }
    CHECK(xd_gpu_choose(&gpus, false, XD_GPU_PREF_AUTO) == -1);
    CHECK(xd_gpu_choose(&gpus, true, XD_GPU_PREF_AUTO) == 1);
    CHECK(xd_gpu_choose(&gpus, true, XD_GPU_PREF_DEFAULT) == -1);
    CHECK(xd_gpu_choose(&gpus, false, XD_GPU_PREF_OTHER) == 1);
    xd_gpu_list one = {.items = gpus.items, .count = 1};
    CHECK(xd_gpu_choose(&one, true, XD_GPU_PREF_OTHER) == -1);
    xd_gpu_list_free(&gpus);
    CHECK(gpus.count == 0);
    xd_gpu_list_parse(nullptr, &gpus);
    CHECK(gpus.count == 0);
    /* Without switcheroo-control (never watched here): one card. */
    CHECK(xd_gpu_current()->count == 0);

    char *root = g_dir_make_tmp("xd-gpu-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    char *path = g_build_filename(root, "xeondesktop", "gpu.ini", nullptr);
    CHECK(xd_gpu_pref_get(path, "game.desktop") == XD_GPU_PREF_AUTO);
    CHECK(xd_gpu_pref_set(path, "game.desktop", XD_GPU_PREF_OTHER));
    CHECK(xd_gpu_pref_set(path, "browser.desktop", XD_GPU_PREF_DEFAULT));
    CHECK(xd_gpu_pref_get(path, "game.desktop") == XD_GPU_PREF_OTHER);
    CHECK(xd_gpu_pref_get(path, "browser.desktop") == XD_GPU_PREF_DEFAULT);
    CHECK(file_has(path, "game.desktop=dedicated"));
    CHECK(xd_gpu_pref_set(path, "game.desktop", XD_GPU_PREF_AUTO));
    CHECK(xd_gpu_pref_get(path, "game.desktop") == XD_GPU_PREF_AUTO && !file_has(path, "game"));
    g_free(path);
    rm_tree(root);
    g_free(root);
}

static void test_search_match(void) {
    xd_search_name search_name;
    xd_search_name_init(&search_name, "  ");
    CHECK(xd_search_name_matches(&search_name, "anything"));
    xd_search_name_clear(&search_name);
    xd_search_name_init(&search_name, "Report");
    CHECK(xd_search_name_matches(&search_name, "Annual report 2026.odt"));
    CHECK(xd_search_name_matches(&search_name, "REPORT"));
    CHECK(!xd_search_name_matches(&search_name, "Repo.txt"));
    xd_search_name_clear(&search_name);
    xd_search_name_init(&search_name, "*.TXT");
    CHECK(xd_search_name_matches(&search_name, "notes.txt"));
    CHECK(!xd_search_name_matches(&search_name, "notes.txt.bak"));
    xd_search_name_clear(&search_name);
    xd_search_name_init(&search_name, "l?tter*");
    CHECK(xd_search_name_matches(&search_name, "Letter.txt"));
    CHECK(!xd_search_name_matches(&search_name, "a Letter.txt"));
    xd_search_name_clear(&search_name);
    xd_search_name_init(&search_name, "\u00FCn\u00EF");
    CHECK(xd_search_name_matches(&search_name, "\u00DCN\u00CFCODE"));
    xd_search_name_clear(&search_name);
    CHECK(search_name.part == nullptr && search_name.glob == nullptr);

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
    char *path = xd_thumb_path("/c", "file:///home/jens/photos/me.png", XD_THUMB_NORMAL);
    CHECK(strcmp(path, "/c/normal/c6ee772d9e49320e97ec29a7eb5b1697.png") == 0);
    g_free(path);
    path = xd_thumb_path("/c", "file:///home/jens/photos/me.png", XD_THUMB_XXLARGE);
    CHECK(strcmp(path, "/c/xx-large/c6ee772d9e49320e97ec29a7eb5b1697.png") == 0);
    g_free(path);
    path = xd_thumb_fail_path("/c", "file:///home/jens/photos/me.png");
    CHECK(strcmp(path, "/c/fail/xd-thumbnailer-" XD_VERSION
                       "/c6ee772d9e49320e97ec29a7eb5b1697.png") == 0);
    g_free(path);
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
    GPtrArray *array = xd_favorites_load(path);
    CHECK(array->len == 3);
    if (array->len == 3) {
        XdFavorite *first = g_ptr_array_index(array, 0), *second = g_ptr_array_index(array, 1),
                   *third = g_ptr_array_index(array, 2);
        CHECK(strcmp(first->uri, "file:///home/me/My%20Work") == 0);
        CHECK(g_strcmp0(first->label, "Work stuff") == 0);
        CHECK(second->label == nullptr);
        char *name = xd_favorite_name(second);
        CHECK(strcmp(name, "data") == 0);
        g_free(name);
        name = xd_favorite_name(third);
        CHECK(strcmp(name, "server") == 0);
        g_free(name);
        g_clear_pointer(&first->label, g_free);
        name = xd_favorite_name(first);
        CHECK(strcmp(name, "My Work") == 0);
        g_free(name);
    }
    g_ptr_array_unref(array);

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
    XdLauncher launcher;
    CHECK(xd_launcher_read(path, &launcher, nullptr));
    CHECK(g_strcmp0(launcher.target, "editor %F") == 0 &&
          g_strcmp0(launcher.start_in, "/srv") == 0);
    CHECK(g_strcmp0(launcher.comment, "Edits text") == 0 && !launcher.is_link);
    CHECK(g_strcmp0(launcher.icon, "accessories-text-editor") == 0);
    g_free(launcher.target);
    launcher.target = g_strdup("editor --new-window %F");
    g_free(launcher.start_in);
    launcher.start_in = g_strdup("");
    g_free(launcher.comment);
    launcher.comment = g_strdup("Writes notes");
    /* Read-only (a system launcher): the change goes to a user copy. Not
     * as root (package builds in containers), which writes anyway. */
    CHECK(g_chmod(sys, 0555) == 0);
    bool read_only = access(sys, W_OK) != 0;
    if (!read_only)
        g_printerr("test_launcher_fields: running as root, skipped the read-only launcher\n");
    char *written = read_only ? xd_launcher_write(path, &launcher, user, nullptr) : nullptr;
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
    char *again = xd_launcher_write(path, &launcher, user, nullptr);
    CHECK(g_strcmp0(again, path) == 0);
    g_free(again);
    /* Not trusted to run until made executable, which rewriting keeps;
     * never through a symbolic link. */
    CHECK(!xd_launcher_is_trusted(path));
    CHECK(xd_launcher_set_trusted(path, nullptr) && xd_launcher_is_trusted(path));
    again = xd_launcher_write(path, &launcher, user, nullptr);
    struct stat st;
    CHECK(g_strcmp0(again, path) == 0 && stat(path, &st) == 0 && (st.st_mode & S_IXUSR));
    g_free(again);
    char *alias = g_build_filename(sys, "alias.desktop", nullptr);
    CHECK(symlink(path, alias) == 0);
    GError *err = nullptr;
    CHECK(!xd_launcher_set_trusted(alias, &err) && err != nullptr);
    g_clear_error(&err);
    CHECK(g_remove(alias) == 0);
    g_free(alias);
    xd_launcher_clear(&launcher);

    char *dir_entry = g_build_filename(sys, "folder.desktop", nullptr);
    CHECK(g_file_set_contents(dir_entry, "[Desktop Entry]\nType=Directory\nName=X\n", -1,
                              nullptr));
    CHECK(!xd_launcher_read(dir_entry, &launcher, nullptr));
    char *link = g_build_filename(sys, "site.desktop", nullptr);
    CHECK(g_file_set_contents(link, "[Desktop Entry]\nType=Link\nName=Site\nURL=https://a.b/\n",
                              -1, nullptr));
    CHECK(xd_launcher_read(link, &launcher, nullptr) && launcher.is_link &&
          g_strcmp0(launcher.target, "https://a.b/") == 0);
    xd_launcher_clear(&launcher);

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

/* Disk Cleanup on a scratch drive: which places are the drive's, what
 * each category finds, and that deleting takes exactly that. */
static gboolean cleanup_has(const XdCleanupCategory *cleanup_category, const char *root,
                            const char *rel) {
    char *path = g_build_filename(root, rel, nullptr);
    gboolean has =
        g_ptr_array_find_with_equal_func(cleanup_category->items, path, g_str_equal, nullptr);
    g_free(path);
    return has;
}

static const XdCleanupCategory *cleanup_find(GPtrArray *cats, XdCleanupKind kind) {
    for (guint i = 0; i < cats->len; i++) {
        const XdCleanupCategory *cleanup_category = g_ptr_array_index(cats, i);
        if (cleanup_category->kind == kind)
            return cleanup_category;
    }
    return nullptr;
}

static gboolean exists_at(const char *root, const char *rel) {
    char *path = g_build_filename(root, rel, nullptr);
    struct stat st;
    gboolean there = lstat(path, &st) == 0;
    g_free(path);
    return there;
}

static void set_times_at(const char *root, const char *rel, gint64 seconds) {
    char *path = g_build_filename(root, rel, nullptr);
    struct timespec time[2] = {{.tv_sec = (time_t)seconds}, {.tv_sec = (time_t)seconds}};
    CHECK(utimensat(AT_FDCWD, path, time, AT_SYMLINK_NOFOLLOW) == 0);
    g_free(path);
}

/* Encrypted volumes: names, items, remembered passphrases' attributes,
 * crypttab names, and telling a wrong passphrase from other failures. */
static void test_luks(void) {
    /* gvfs's and GNOME Disks' schema, so remembered passphrases are shared. */
    CHECK(strcmp(XD_LUKS_SECRET_SCHEMA, "org.gnome.GVfs.Luks.Password") == 0);
    CHECK(strcmp(XD_LUKS_SECRET_ATTRIBUTE, "gvfs-luks-uuid") == 0);
    char *label = xd_luks_secret_label("Private");
    CHECK(strcmp(label, "Encryption passphrase for Private") == 0);
    g_free(label);

    label = xd_luks_item_uri("1234-ab cd");
    CHECK(strcmp(label, "computer:///encrypted/1234-ab%20cd") == 0);
    char *uuid = xd_luks_item_uuid(label);
    CHECK(uuid != nullptr && strcmp(uuid, "1234-ab cd") == 0);
    g_free(uuid);
    g_free(label);
    CHECK(xd_luks_item_uuid("computer:///encrypted/") == nullptr);
    CHECK(xd_luks_item_uuid("computer:///volume/abc") == nullptr);
    CHECK(xd_luks_item_uuid("computer:///encrypted/a%2Fb") == nullptr); /* no slashes in it */
    CHECK(xd_luks_item_uuid(nullptr) == nullptr);

    label = xd_luks_volume_name("Private", "Header", "7.45 GB");
    CHECK(strcmp(label, "Private") == 0);
    g_free(label);
    label = xd_luks_volume_name("", "Header", "7.45 GB");
    CHECK(strcmp(label, "Header") == 0);
    g_free(label);
    label = xd_luks_volume_name("", "", "7.45 GB");
    CHECK(strcmp(label, "7.45 GB Encrypted Volume") == 0);
    g_free(label);
    label = xd_luks_volume_name(nullptr, nullptr, nullptr);
    CHECK(strcmp(label, "Encrypted Volume") == 0);
    g_free(label);
    label = xd_luks_version_label("2");
    CHECK(strcmp(label, "LUKS2") == 0);
    g_free(label);
    label = xd_luks_version_label("");
    CHECK(strcmp(label, "LUKS") == 0);
    g_free(label);

    CHECK(xd_luks_passphrase_problem("a b", "a b") == nullptr);
    CHECK(xd_luks_passphrase_problem("", "") != nullptr);
    CHECK(xd_luks_passphrase_problem("one", "One") != nullptr);
    CHECK(xd_luks_passphrase_problem("one", nullptr) != nullptr);

    const char *failed = "org.freedesktop.UDisks2.Error.Failed";
    CHECK(xd_luks_wrong_passphrase(failed, "Error unlocking /dev/sdb1: Failed to activate "
                                           "device: Operation not permitted"));
    CHECK(xd_luks_wrong_passphrase(failed, "Error unlocking /dev/sde1: Failed to activate "
                                           "device: Incorrect passphrase."));
    CHECK(xd_luks_wrong_passphrase(failed, "Error changing passphrase on device /dev/sde1: "
                                           "No key available with this passphrase."));
    CHECK(!xd_luks_wrong_passphrase(failed, "Error unlocking /dev/sdb1: Failed to activate "
                                            "device: No such device"));
    CHECK(!xd_luks_wrong_passphrase("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed",
                                    "Operation not permitted"));
    CHECK(!xd_luks_wrong_passphrase(nullptr, nullptr));

    label = xd_crypttab_name("uuid-1");
    CHECK(strcmp(label, "luks-uuid-1") == 0);
    char *key = xd_crypttab_key_path(label);
    CHECK(strcmp(key, "/etc/luks-keys/luks-uuid-1") == 0);
    g_free(key);
    g_free(label);

    char *pass = g_strdup("secret");
    xd_luks_passphrase_free(pass);
    xd_luks_passphrase_free(nullptr);
}

static void test_disk_cleanup(void) {
    char *text = xd_cleanup_format_kb(0);
    CHECK(g_str_equal(text, "0 KB"));
    g_free(text);
    text = xd_cleanup_format_kb(1);
    CHECK(g_str_equal(text, "1 KB"));
    g_free(text);
    text = xd_cleanup_format_kb(1025);
    CHECK(g_str_equal(text, "2 KB"));
    g_free(text);
    text = xd_cleanup_format_kb(12345678);
    CHECK(g_str_equal(text, "12,057 KB"));
    g_free(text);
    text = xd_cleanup_format_kb(1024ULL * 1000000);
    CHECK(g_str_equal(text, "1,000,000 KB"));
    g_free(text);

    char *root = g_dir_make_tmp("xd-cleanup-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    uid_t uid = getuid();
    char *drive = g_build_filename(root, "drive", nullptr);
    char *usb = g_build_filename(drive, "media", "usb", nullptr);
    char *data = g_build_filename(drive, "home", ".local", "share", nullptr);
    char *cache = g_build_filename(drive, "home", ".cache", nullptr);
    char *tmp = g_build_filename(drive, "tmp", nullptr);
    char *usb_trash = g_strdup_printf("media/usb/.Trash-%u/files/z.txt", (unsigned)uid);
    const char *files[] = {
        "home/.local/share/Trash/files/a.txt",
        "home/.local/share/Trash/files/dir/b.txt",
        "home/.local/share/Trash/info/a.txt.trashinfo",
        "home/.local/share/Trash/info/dir.trashinfo",
        "home/.cache/thumbnails/normal/x.png",
        "home/.cache/thumbnails/fail/gnome-thumbnail-factory/y.png",
        "home/.cache/mozilla/firefox/abc.default/cache2/entries/E1",
        "home/.cache/mozilla/firefox/abc.default/keep.txt",
        "home/.cache/chromium/Default/Cache/data_0",
        "home/.cache/chromium/Default/Preferences",
        "home/.cache/other/keep",
        "tmp/old.txt",
        "tmp/new.txt",
        "tmp/olddir/inside.txt",
        "outside/precious",
        usb_trash,
    };
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++)
        put_file(drive, files[i], "some bytes\n");
    char *fifo_dir = g_build_filename(tmp, "pipes", nullptr);
    char *fifo = g_build_filename(fifo_dir, "fifo", nullptr);
    CHECK(g_mkdir(fifo_dir, 0700) == 0 && mkfifo(fifo, 0600) == 0);
    char *link = g_build_filename(tmp, "link", nullptr);
    char *precious = g_build_filename(drive, "outside", "precious", nullptr);
    CHECK(symlink(precious, link) == 0);
    /* A read-only folder of the user's in the Recycle Bin still goes. */
    char *ro = g_build_filename(data, "Trash", "files", "dir", nullptr);
    CHECK(g_chmod(ro, 0555) == 0);

    /* "Now" is a month on, so what's untouched since today is old;
     * new.txt was touched in the future, so it's recent. */
    gint64 real = g_get_real_time() / G_USEC_PER_SEC;
    gint64 now = (real + 30 * 24 * 3600) * G_USEC_PER_SEC;
    set_times_at(drive, "tmp/old.txt", real - 30 * 24 * 3600);
    set_times_at(drive, "tmp/olddir/inside.txt", real - 30 * 24 * 3600);
    set_times_at(drive, "tmp/olddir", real - 30 * 24 * 3600);
    set_times_at(drive, "tmp/new.txt", real + 30 * 24 * 3600);

    const char *roots[] = {drive, usb, nullptr};
    const char *temps[] = {tmp, "/nonexistent-xd-tmp", nullptr};
    XdCleanupPlaces *cleanup_places = xd_cleanup_places_new(drive, roots, data, cache, nullptr, temps, uid);
    CHECK(g_strv_length(cleanup_places->trashes) == 1 &&
          g_str_has_suffix(cleanup_places->trashes[0], "share/Trash"));
    CHECK(cleanup_places->thumbnails != nullptr &&
          g_str_has_suffix(cleanup_places->thumbnails, ".cache/thumbnails"));
    CHECK(g_strv_length(cleanup_places->browser_caches) == 2 &&
          g_str_has_suffix(cleanup_places->browser_caches[0], "chromium/Default/Cache") &&
          g_str_has_suffix(cleanup_places->browser_caches[1], "firefox/abc.default/cache2"));
    CHECK(g_strv_length(cleanup_places->temp_dirs) == 1 &&
          g_str_equal(cleanup_places->temp_dirs[0], tmp));

    /* The USB drive has only its own trash. */
    XdCleanupPlaces *pu = xd_cleanup_places_new(usb, roots, data, cache, nullptr, temps, uid);
    CHECK(g_strv_length(pu->trashes) == 1 &&
          strstr(pu->trashes[0], "/media/usb/.Trash-") != nullptr);
    CHECK(pu->thumbnails == nullptr && pu->browser_caches[0] == nullptr &&
          pu->temp_dirs[0] == nullptr);
    GPtrArray *ucats = xd_cleanup_scan(pu, uid, now, nullptr, nullptr);
    CHECK(ucats->len == 1);
    const XdCleanupCategory *ut = cleanup_find(ucats, XD_CLEANUP_TRASH);
    CHECK(ut != nullptr && ut->items->len == 1 && ut->size > 0);

    gint scanning = -1;
    GPtrArray *cats = xd_cleanup_scan(cleanup_places, uid, now, nullptr, &scanning);
    /* The four of XP's, and what's left in the cache folder. */
    CHECK(cats->len == 5 && scanning == XD_CLEANUP_PROGRAMS);
    const XdCleanupCategory *programs = cleanup_find(cats, XD_CLEANUP_PROGRAMS);
    CHECK(programs != nullptr && programs->items->len == 1 && !programs->checked &&
          cleanup_has(programs, drive, "home/.cache/other"));
    const XdCleanupCategory *web = cleanup_find(cats, XD_CLEANUP_INTERNET);
    const XdCleanupCategory *trash = cleanup_find(cats, XD_CLEANUP_TRASH);
    const XdCleanupCategory *temp = cleanup_find(cats, XD_CLEANUP_TEMP);
    const XdCleanupCategory *thumbs = cleanup_find(cats, XD_CLEANUP_THUMBNAILS);
    CHECK(web != nullptr && web->items->len == 2 && web->checked && web->size > 0 &&
          cleanup_has(web, drive, "home/.cache/mozilla/firefox/abc.default/cache2/entries") &&
          cleanup_has(web, drive, "home/.cache/chromium/Default/Cache/data_0"));
    CHECK(trash != nullptr && trash->items->len == 4 && !trash->checked &&
          g_strcmp0(trash->view, "trash:///") == 0);
    /* Old things, and the link (not what it points at); not the new
     * file, nor the folder with a FIFO in it. */
    CHECK(temp != nullptr && temp->items->len == 3 && !temp->checked &&
          cleanup_has(temp, drive, "tmp/old.txt") && cleanup_has(temp, drive, "tmp/olddir") &&
          cleanup_has(temp, drive, "tmp/link") && !cleanup_has(temp, drive, "tmp/new.txt") &&
          !cleanup_has(temp, drive, "tmp/pipes"));
    CHECK(thumbs != nullptr && thumbs->items->len == 2 && thumbs->checked &&
          cleanup_has(thumbs, drive, "home/.cache/thumbnails/normal/x.png") &&
          cleanup_has(thumbs, drive, "home/.cache/thumbnails/fail/gnome-thumbnail-factory"));

    /* Something that isn't in the category's folders is never deleted. */
    XdCleanupCategory rogue = *temp;
    rogue.items = g_ptr_array_new();
    g_ptr_array_add(rogue.items, precious);
    GError *err = nullptr;
    CHECK(!xd_cleanup_delete(&rogue, uid, now, nullptr, &err) && err != nullptr);
    g_clear_error(&err);
    g_ptr_array_unref(rogue.items);
    CHECK(exists_at(drive, "outside/precious"));

    for (guint i = 0; i < cats->len; i++) {
        CHECK(xd_cleanup_delete(g_ptr_array_index(cats, i), uid, now, nullptr, &err));
        if (err != nullptr) {
            fprintf(stderr, "cleanup: %s\n", err->message);
            g_clear_error(&err);
        }
    }
    CHECK(!exists_at(drive, "home/.local/share/Trash/files/a.txt") &&
          !exists_at(drive, "home/.local/share/Trash/files/dir") &&
          !exists_at(drive, "home/.local/share/Trash/info/dir.trashinfo") &&
          exists_at(drive, "home/.local/share/Trash/files") &&
          exists_at(drive, "home/.local/share/Trash/info"));
    CHECK(!exists_at(drive, "home/.cache/thumbnails/normal/x.png") &&
          exists_at(drive, "home/.cache/thumbnails/normal") &&
          !exists_at(drive, "home/.cache/thumbnails/fail/gnome-thumbnail-factory"));
    CHECK(!exists_at(drive, "home/.cache/mozilla/firefox/abc.default/cache2/entries") &&
          exists_at(drive, "home/.cache/mozilla/firefox/abc.default/cache2") &&
          exists_at(drive, "home/.cache/mozilla/firefox/abc.default/keep.txt") &&
          !exists_at(drive, "home/.cache/chromium/Default/Cache/data_0") &&
          exists_at(drive, "home/.cache/chromium/Default/Preferences") &&
          !exists_at(drive, "home/.cache/other"));
    CHECK(!exists_at(drive, "tmp/old.txt") && !exists_at(drive, "tmp/olddir") &&
          !exists_at(drive, "tmp/link") && exists_at(drive, "tmp/new.txt") &&
          exists_at(drive, "tmp/pipes/fifo") && exists_at(drive, "outside/precious"));
    /* The other drive's trash wasn't this drive's to empty. */
    CHECK(exists_at(drive, usb_trash));

    g_ptr_array_unref(cats);
    g_ptr_array_unref(ucats);
    xd_cleanup_places_free(cleanup_places);
    xd_cleanup_places_free(pu);
    rm_tree(root);
    g_free(link);
    g_free(precious);
    g_free(fifo);
    g_free(fifo_dir);
    g_free(ro);
    g_free(usb_trash);
    g_free(tmp);
    g_free(cache);
    g_free(data);
    g_free(usb);
    g_free(drive);
    g_free(root);
}

/* Disk Cleanup's own categories for Linux: shader, developer tools' and
 * programs' caches, each folder's contents (not the folder). */
static void test_disk_cleanup_caches(void) {
    char *root = g_dir_make_tmp("xd-cleanup-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    uid_t uid = getuid();
    char *home = g_build_filename(root, "home", nullptr);
    char *data = g_build_filename(home, ".local", "share", nullptr);
    char *cache = g_build_filename(home, ".cache", nullptr);
    const char *files[] = {
        "home/.cache/mesa_shader_cache/ab/cd",
        "home/.cache/nvidia/GLCache/x/y",
        "home/.cache/nvidia/settings.keep",
        "home/.nv/GLCache/z",
        "home/.cache/pip/http/w",
        "home/.npm/_cacache/index",
        "home/.npm/keep",
        "home/.cargo/registry/cache/crate",
        "home/.cargo/registry/index/keep",
        "home/.cache/some-program/data",
        "home/.cache/another/file",
        "home/.cache/thumbnails/normal/t.png",
        "home/.cache/mozilla/firefox/p/startupCache/s",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++)
        put_file(root, files[i], "some bytes\n");
    const char *roots[] = {root, nullptr};
    const char *temps[] = {nullptr};
    gint64 now = g_get_real_time();
    XdCleanupPlaces *places = xd_cleanup_places_new(root, roots, data, cache, home, temps, uid);
    CHECK(g_strv_length(places->shader_caches) == 3 &&
          g_strv_length(places->developer_caches) == 3 &&
          g_strcmp0(places->program_caches, cache) == 0);
    CHECK(xd_cleanup_places_holds(places, home));
    GPtrArray *cats = xd_cleanup_scan(places, uid, now, nullptr, nullptr);
    const XdCleanupCategory *shaders = cleanup_find(cats, XD_CLEANUP_SHADERS);
    const XdCleanupCategory *developer = cleanup_find(cats, XD_CLEANUP_DEVELOPER);
    const XdCleanupCategory *programs = cleanup_find(cats, XD_CLEANUP_PROGRAMS);
    CHECK(shaders != nullptr && shaders->items->len == 3 && !shaders->checked &&
          cleanup_has(shaders, root, "home/.cache/mesa_shader_cache/ab") &&
          cleanup_has(shaders, root, "home/.cache/nvidia/GLCache/x") &&
          cleanup_has(shaders, root, "home/.nv/GLCache/z"));
    CHECK(developer != nullptr && developer->items->len == 3 &&
          cleanup_has(developer, root, "home/.cache/pip/http") &&
          cleanup_has(developer, root, "home/.npm/_cacache/index") &&
          cleanup_has(developer, root, "home/.cargo/registry/cache/crate"));
    /* Not what the other categories take: thumbnails, browsers', shader
     * and developer tools' folders. */
    CHECK(programs != nullptr && programs->items->len == 2 &&
          cleanup_has(programs, root, "home/.cache/some-program") &&
          cleanup_has(programs, root, "home/.cache/another"));
    GError *err = nullptr;
    for (guint i = 0; i < cats->len; i++) {
        const XdCleanupCategory *cleanup_category = g_ptr_array_index(cats, i);
        if (cleanup_category->kind >= XD_CLEANUP_SHADERS) {
            CHECK(xd_cleanup_delete(cleanup_category, uid, now, nullptr, &err));
            g_clear_error(&err);
        }
    }
    CHECK(!exists_at(root, "home/.cache/mesa_shader_cache/ab") &&
          exists_at(root, "home/.cache/mesa_shader_cache") &&
          !exists_at(root, "home/.nv/GLCache/z") && exists_at(root, "home/.cache/nvidia") &&
          exists_at(root, "home/.cache/nvidia/settings.keep"));
    CHECK(!exists_at(root, "home/.npm/_cacache/index") && exists_at(root, "home/.npm/keep") &&
          exists_at(root, "home/.cargo/registry/index/keep") &&
          !exists_at(root, "home/.cache/some-program") &&
          exists_at(root, "home/.cache/thumbnails/normal/t.png") &&
          exists_at(root, "home/.cache/mozilla/firefox/p/startupCache/s"));
    /* An empty category isn't listed. */
    GPtrArray *again = xd_cleanup_scan(places, uid, now, nullptr, nullptr);
    CHECK(cleanup_find(again, XD_CLEANUP_SHADERS) == nullptr &&
          cleanup_find(again, XD_CLEANUP_DEVELOPER) == nullptr);
    CHECK(xd_cleanup_kind_is_system(XD_CLEANUP_LOGS) &&
          !xd_cleanup_kind_is_system(XD_CLEANUP_FLATPAK) &&
          g_str_equal(xd_cleanup_kind_title(XD_CLEANUP_SHADERS), "Graphics shader caches"));
    g_ptr_array_unref(again);
    g_ptr_array_unref(cats);
    xd_cleanup_places_free(places);
    rm_tree(root);
    g_free(cache);
    g_free(data);
    g_free(home);
    g_free(root);
}

/* The bytes on disk of the files named (under `root`). */
static guint64 disk_bytes(const char *root, const char *const *rels) {
    guint64 size = 0;
    for (const char *const *rel = rels; *rel != nullptr; rel++) {
        char *path = g_build_filename(root, *rel, nullptr);
        struct stat st;
        if (lstat(path, &st) == 0)
            size += (guint64)st.st_blocks * 512;
        g_free(path);
    }
    return size;
}

static void put_script(const char *dir, const char *name, const char *body) {
    char *path = g_build_filename(dir, name, nullptr);
    char *text = g_strdup_printf("#!/bin/sh\n%s", body);
    CHECK(g_file_set_contents(path, text, -1, nullptr) && g_chmod(path, 0755) == 0);
    g_free(text);
    g_free(path);
}

/* Disk Cleanup's system files, against a scratch root and stand-in
 * tools: pacman's cache but the installed versions, archived journal
 * files over two weeks old, crash dumps; never through a link. */
static void test_disk_cleanup_system(void) {
    char *root = g_dir_make_tmp("xd-syscleanup-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    char *tools = g_build_filename(root, "tools", nullptr);
    char *log = g_build_filename(root, "calls", nullptr);
    CHECK(g_mkdir(tools, 0755) == 0);
    char *pacman = g_strdup_printf(
        "case \"$1\" in -Q) printf 'foo 1.0-1\\nbar 2:3.0-2\\n';; *) echo \"pacman $*\" >>'%s';; "
        "esac\n",
        log);
    put_script(tools, "pacman", pacman);
    put_script(tools, "pacman-conf", "echo /var/cache/pacman/pkg/\n");
    char *journalctl = g_strdup_printf("echo \"journalctl $*\" >>'%s'\n", log);
    put_script(tools, "journalctl", journalctl);
    const char *files[] = {
        "var/cache/pacman/pkg/foo-1.0-1-x86_64.pkg.tar.zst",
        "var/cache/pacman/pkg/foo-0.9-1-x86_64.pkg.tar.zst",
        "var/cache/pacman/pkg/foo-0.9-1-x86_64.pkg.tar.zst.sig",
        "var/cache/pacman/pkg/bar-2:3.0-2-any.pkg.tar.zst",
        "var/cache/pacman/pkg/notes.txt",
        "var/log/journal/abc/system@0001-0002.journal",
        "var/log/journal/abc/user-1000@0003-0004.journal~",
        "var/log/journal/abc/system@0005-0006.journal",
        "var/log/journal/abc/system.journal",
        "var/lib/systemd/coredump/core.prog.1000.abc.zst",
        "var/lib/systemd/coredump/core.other.1000.def.zst",
        "var/lib/systemd/coredump/README",
        "outside/precious",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++)
        put_file(root, files[i], "some bytes for the cache\n");
    char *precious = g_build_filename(root, "outside", "precious", nullptr);
    char *link = g_build_filename(root, "var/lib/systemd/coredump/core.link", nullptr);
    CHECK(symlink(precious, link) == 0);
    gint64 real = g_get_real_time() / G_USEC_PER_SEC;
    set_times_at(root, "var/log/journal/abc/system@0001-0002.journal", real - 30 * 24 * 3600);
    set_times_at(root, "var/log/journal/abc/user-1000@0003-0004.journal~", real - 30 * 24 * 3600);
    set_times_at(root, "var/log/journal/abc/system.journal", real - 30 * 24 * 3600);

    xd_sysclean_env env = {.root = root, .tools = tools, .now = g_get_real_time()};
    XdCleanupCategory *packages = xd_sysclean_scan(XD_CLEANUP_PACKAGES, &env, nullptr);
    XdCleanupCategory *logs = xd_sysclean_scan(XD_CLEANUP_LOGS, &env, nullptr);
    XdCleanupCategory *crashes = xd_sysclean_scan(XD_CLEANUP_CRASHES, &env, nullptr);
    const char *unneeded[] = {"var/cache/pacman/pkg/foo-0.9-1-x86_64.pkg.tar.zst",
                              "var/cache/pacman/pkg/foo-0.9-1-x86_64.pkg.tar.zst.sig", nullptr};
    const char *old_logs[] = {"var/log/journal/abc/system@0001-0002.journal",
                              "var/log/journal/abc/user-1000@0003-0004.journal~", nullptr};
    const char *dumps[] = {"var/lib/systemd/coredump/core.prog.1000.abc.zst",
                           "var/lib/systemd/coredump/core.other.1000.def.zst", nullptr};
    CHECK(packages != nullptr && packages->size == disk_bytes(root, unneeded) &&
          packages->checked && g_str_has_suffix(packages->view, "/var/cache/pacman/pkg/"));
    CHECK(logs != nullptr && logs->size == disk_bytes(root, old_logs) && !logs->checked);
    CHECK(crashes != nullptr && crashes->size == disk_bytes(root, dumps));

    GError *err = nullptr;
    CHECK(xd_sysclean_clean(XD_CLEANUP_PACKAGES, &env, &err) &&
          xd_sysclean_clean(XD_CLEANUP_LOGS, &env, &err) &&
          xd_sysclean_clean(XD_CLEANUP_CRASHES, &env, &err));
    g_clear_error(&err);
    char *calls = nullptr;
    CHECK(g_file_get_contents(log, &calls, nullptr, nullptr) &&
          g_str_equal(calls, "pacman -Sc --noconfirm\njournalctl --vacuum-time=2weeks\n"));
    CHECK(!exists_at(root, "var/lib/systemd/coredump/core.prog.1000.abc.zst") &&
          exists_at(root, "var/lib/systemd/coredump/README") &&
          exists_at(root, "var/lib/systemd/coredump/core.link") &&
          exists_at(root, "outside/precious"));
    /* Nothing left: no crash dumps listed; no package manager, no cache. */
    XdCleanupCategory *none = xd_sysclean_scan(XD_CLEANUP_CRASHES, &env, nullptr);
    CHECK(none == nullptr);
    xd_sysclean_env bare = {.root = root, .tools = log, .now = env.now};
    CHECK(xd_sysclean_scan(XD_CLEANUP_PACKAGES, &bare, nullptr) == nullptr);
    CHECK(!xd_sysclean_clean(XD_CLEANUP_PACKAGES, &bare, &err) && err != nullptr);
    g_clear_error(&err);
    XdCleanupKind kind;
    CHECK(xd_sysclean_kind_from_name("logs", &kind) && kind == XD_CLEANUP_LOGS &&
          !xd_sysclean_kind_from_name("../etc", &kind) &&
          g_str_equal(xd_sysclean_name(XD_CLEANUP_CRASHES), "crashes"));

    /* As the categories xd_cleanup_scan's arrays hold, they free there. */
    const char *roots[] = {root, nullptr};
    const char *temps[] = {nullptr};
    XdCleanupPlaces *places =
        xd_cleanup_places_new(root, roots, nullptr, nullptr, nullptr, temps, getuid());
    GPtrArray *all = xd_cleanup_scan(places, getuid(), env.now, nullptr, nullptr);
    g_ptr_array_add(all, packages);
    g_ptr_array_add(all, logs);
    g_ptr_array_add(all, crashes);
    g_ptr_array_unref(all);
    xd_cleanup_places_free(places);
    g_free(calls);
    rm_tree(root);
    g_free(link);
    g_free(precious);
    g_free(journalctl);
    g_free(pacman);
    g_free(log);
    g_free(tools);
    g_free(root);
}

/* Date and Time's Internet Time tab. */
static void test_timedate_zone(void) {
    /* The map's band: standard time, summer time or not, either hemisphere. */
    static const struct {
        const char *zone;
        double hours;
    } zones[] = {
        {"Europe/London", 0}, {"America/New_York", -5}, {"Australia/Sydney", 10},
        {"Asia/Kolkata", 5.5}, {"UTC", 0},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(zones); i++) {
        GTimeZone *tz = g_time_zone_new_identifier(zones[i].zone);
        CHECK(tz != nullptr);
        if (tz == nullptr)
            continue;
        GTimeSpan offset = xd_zone_standard_offset(tz, 2026);
        CHECK(offset == (GTimeSpan)(zones[i].hours * (double)G_TIME_SPAN_HOUR));
        g_time_zone_unref(tz);
    }
}

static void test_timedate_ntp(void) {
    /* timesyncd's order (runtime, timesyncd.conf, network, fallback), each
     * server once. */
    const char *runtime[] = {"ntp.example.com", nullptr};
    const char *system[] = {"time.example.org", "NTP.example.com", "", nullptr};
    const char *fallback[] = {"0.arch.pool.ntp.org", "time.example.org", nullptr};
    char **list = xd_ntp_server_list(runtime, nullptr, system, fallback);
    CHECK(g_strv_length(list) == 3);
    CHECK(g_str_equal(list[0], "ntp.example.com") && g_str_equal(list[1], "time.example.org") &&
          g_str_equal(list[2], "0.arch.pool.ntp.org"));
    g_strfreev(list);
    list = xd_ntp_server_list(nullptr, nullptr, nullptr, nullptr);
    CHECK(list != nullptr && list[0] == nullptr);
    g_strfreev(list);

    /* NTPMessage's DestinationTimestamp; nothing from anything else. */
    const guint8 ref[] = {'G', 'P', 'S', 0};
    GVariant *msg = g_variant_ref_sink(g_variant_new(
        "(uuuuitt@ayttttbtt)", 0u, 4u, 4u, 2u, -23, (guint64)1000, (guint64)2000,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, ref, sizeof(ref), 1), (guint64)1,
        (guint64)2, (guint64)3, (guint64)1792342436704110, FALSE, (guint64)1, (guint64)0));
    CHECK(xd_ntp_message_received(msg) == 1792342436704110);
    g_variant_unref(msg);
    GVariant *other = g_variant_ref_sink(g_variant_new("(t)", (guint64)5));
    CHECK(xd_ntp_message_received(other) == 0);
    CHECK(xd_ntp_message_received(nullptr) == 0);
    g_variant_unref(other);

    /* XP's words, the locale's short date and time (C here). */
    GDateTime *when = g_date_time_new_utc(2026, 10, 18, 9, 5, 0);
    check_text(xd_ntp_synced_text("time.example.org", when),
               "The time has been successfully synchronized with time.example.org on 10/18/26 "
               "at 9:05 AM.");
    GDateTime *pm = g_date_time_new_utc(2026, 10, 25, 21, 5, 0);
    check_text(xd_ntp_next_text(pm), "Next synchronization: 10/25/26 at 9:05 PM");
    g_date_time_unref(pm);
    g_date_time_unref(when);
    check_text(xd_ntp_failed_text("time.example.org", nullptr),
               "An error occurred while synchronizing with time.example.org.");
    check_text(xd_ntp_failed_text("time.example.org", "Access denied"),
               "An error occurred while synchronizing with time.example.org.\nAccess denied");
}

/* A .trashinfo deleted at `when` (Unix seconds, local time as the spec
 * has it), or with no date if `when` is 0. */
static void put_trashinfo(const char *root, const char *name, gint64 when) {
    char *rel = g_strdup_printf("Trash/info/%s.trashinfo", name);
    GDateTime *dt = when != 0 ? g_date_time_new_from_unix_local(when) : nullptr;
    char *date = dt != nullptr ? g_date_time_format(dt, "%Y-%m-%dT%H:%M:%S") : nullptr;
    char *text = g_strdup_printf("[Trash Info]\nPath=/home/me/%s\n%s%s\n", name,
                                 date != nullptr ? "DeletionDate=" : "",
                                 date != nullptr ? date : "");
    put_file(root, rel, text);
    g_free(text);
    g_free(date);
    if (dt != nullptr)
        g_date_time_unref(dt);
    g_free(rel);
}

static gboolean cleanup_items_has(const XdCleanupCategory *cleanup_category, const char *root,
                                  const char *rel) {
    char *path = g_build_filename(root, rel, nullptr);
    gboolean found =
        g_ptr_array_find_with_equal_func(cleanup_category->items, path, g_str_equal, nullptr);
    g_free(path);
    return found;
}

static void test_privacy(void) {
    /* Recent documents: as GTK, -1 keeps them for ever and 0 not at all. */
    gint64 day = 24 * 60 * 60, timestamp = 1800000000;
    CHECK(xd_recent_shown(true, -1, 0, timestamp));
    CHECK(!xd_recent_shown(false, -1, timestamp, timestamp));
    CHECK(!xd_recent_shown(true, 0, timestamp, timestamp));
    CHECK(xd_recent_shown(true, 7, timestamp - 7 * day, timestamp) &&
          !xd_recent_shown(true, 7, timestamp - 8 * day, timestamp));

    /* Portal permissions: yes/no, location's accuracy and its time kept. */
    const XdPermKind *loc = &xd_perm_kinds[0], *camera = nullptr;
    for (size_t i = 0; i < xd_perm_kind_count; i++)
        if (g_str_equal(xd_perm_kinds[i].id, "camera"))
            camera = &xd_perm_kinds[i];
    CHECK(g_str_equal(loc->table, "location") && camera != nullptr &&
          g_str_equal(camera->table, "devices"));
    if (camera == nullptr)
        return;
    CHECK(xd_perm_allowed(camera, (const char *const[]){"yes", nullptr}));
    CHECK(!xd_perm_allowed(camera, (const char *const[]){"no", nullptr}));
    CHECK(!xd_perm_allowed(camera, (const char *const[]){"ask", nullptr}));
    CHECK(!xd_perm_allowed(camera, (const char *const[]){nullptr}));
    CHECK(xd_perm_allowed(loc, (const char *const[]){"CITY", "1800000000", nullptr}));
    CHECK(!xd_perm_allowed(loc, (const char *const[]){"NONE", "1800000000", nullptr}));
    char **values =
        xd_perm_values_set(loc, (const char *const[]){"EXACT", "1800000000", nullptr}, FALSE);
    CHECK(g_strv_length(values) == 2 && g_str_equal(values[0], "NONE") &&
          g_str_equal(values[1], "1800000000"));
    char maps[] = "org.example.Maps", host[] = "";
    XdPermEntry perm_entry = {.kind = loc, .app = maps, .values = values};
    CHECK(xd_perm_last_used(&perm_entry) == 1800000000);
    g_strfreev(values);
    values = xd_perm_values_set(camera, nullptr, TRUE);
    CHECK(g_strv_length(values) == 1 && g_str_equal(values[0], "yes"));
    perm_entry = (XdPermEntry){.kind = camera, .app = host, .values = values};
    CHECK(xd_perm_last_used(&perm_entry) == -1);
    g_strfreev(values);

    /* Housekeeping: a month's Recycle Bin items by their deletion date,
     * and temporary files untouched as long. "Now" is 40 days on (a
     * file's change time can't be set back). */
    char *root = g_dir_make_tmp("xd-privacy-XXXXXX", nullptr);
    CHECK(root != nullptr);
    if (root == nullptr)
        return;
    uid_t uid = getuid();
    gint64 real = g_get_real_time() / G_USEC_PER_SEC;
    gint64 now = (real + 40 * day) * G_USEC_PER_SEC;
    char *data = g_build_filename(root, "data", nullptr);
    char *tmp = g_build_filename(root, "tmp", nullptr);
    put_file(data, "Trash/files/old.txt", "x");
    put_trashinfo(data, "old.txt", real);
    put_file(data, "Trash/files/olddir/in.txt", "x");
    put_trashinfo(data, "olddir", real);
    put_file(data, "Trash/files/new.txt", "x");
    put_trashinfo(data, "new.txt", real + 35 * day);
    put_file(data, "Trash/files/nodate.txt", "x");
    put_trashinfo(data, "nodate.txt", 0);
    put_trashinfo(data, "gone.txt", real); /* its file went already */
    put_file(data, "Trash/files/noinfo.txt", "x");
    put_file(tmp, "old.txt", "x");
    put_file(tmp, "recent.txt", "x");
    set_times_at(tmp, "old.txt", real - day);
    set_times_at(tmp, "recent.txt", real + 20 * day);

    const char *roots[] = {"/", nullptr};
    const char *temps[] = {tmp, nullptr};
    XdCleanupPlaces *cleanup_places = xd_cleanup_places_new("/", roots, data, nullptr, nullptr, temps, uid);
    GPtrArray *only_trash = xd_cleanup_scan_old(cleanup_places, uid, now, 30 * day * G_USEC_PER_SEC,
                                                TRUE, FALSE, nullptr);
    CHECK(only_trash->len == 1);
    g_ptr_array_unref(only_trash);
    GPtrArray *cats = xd_cleanup_scan_old(cleanup_places, uid, now, 30 * day * G_USEC_PER_SEC, TRUE,
                                          TRUE, nullptr);
    const XdCleanupCategory *trash = cleanup_find(cats, XD_CLEANUP_TRASH);
    const XdCleanupCategory *temp = cleanup_find(cats, XD_CLEANUP_TEMP);
    CHECK(trash != nullptr && trash->items->len == 5 &&
          cleanup_items_has(trash, data, "Trash/files/old.txt") &&
          cleanup_items_has(trash, data, "Trash/info/old.txt.trashinfo") &&
          cleanup_items_has(trash, data, "Trash/files/olddir") &&
          cleanup_items_has(trash, data, "Trash/info/olddir.trashinfo") &&
          cleanup_items_has(trash, data, "Trash/info/gone.txt.trashinfo"));
    CHECK(temp != nullptr && temp->items->len == 1 && cleanup_items_has(temp, tmp, "old.txt"));
    for (guint i = 0; i < cats->len; i++) {
        GError *err = nullptr;
        CHECK(xd_cleanup_delete(g_ptr_array_index(cats, i), uid, now, nullptr, &err));
        g_clear_error(&err);
    }
    CHECK(!exists_at(data, "Trash/files/old.txt") && !exists_at(data, "Trash/files/olddir") &&
          !exists_at(data, "Trash/info/old.txt.trashinfo") &&
          !exists_at(data, "Trash/info/olddir.trashinfo") &&
          !exists_at(data, "Trash/info/gone.txt.trashinfo"));
    CHECK(exists_at(data, "Trash/files/new.txt") && exists_at(data, "Trash/info/new.txt.trashinfo") &&
          exists_at(data, "Trash/files/nodate.txt") && exists_at(data, "Trash/files/noinfo.txt"));
    CHECK(!exists_at(tmp, "old.txt") && exists_at(tmp, "recent.txt"));
    g_ptr_array_unref(cats);
    xd_cleanup_places_free(cleanup_places);
    rm_tree(root);
    g_free(tmp);
    g_free(data);
    g_free(root);
}

/* Input methods: [Keyboard] InputMethod, the variables programs get, the
 * frameworks' own autostart entries, the candidate window's place. */
static void test_input_method(void) {
    xd_settings settings;
    xd_settings_defaults(&settings);
    CHECK(settings.keyboard.input_method[0] == '\0');
    xd_settings_parse(XD_STR("[Keyboard]\nInputMethod=ibus\n"), &settings);
    CHECK(xd_im_from_name(settings.keyboard.input_method) == XD_IM_IBUS);
    xd_strbuf out = {0};
    xd_settings_serialize(&settings, &out);
    CHECK(strstr(xd_strbuf_cstr(&out), "InputMethod=ibus") != nullptr);
    xd_strbuf_free(&out);
    CHECK(xd_im_from_name("fcitx5") == XD_IM_FCITX5);
    CHECK(xd_im_from_name("fcitx") == XD_IM_NONE && xd_im_from_name(nullptr) == XD_IM_NONE);
    CHECK(strcmp(xd_im_name(XD_IM_FCITX5), "fcitx5") == 0 && xd_im_name(XD_IM_NONE)[0] == 0);
    CHECK(xd_im_argv(XD_IM_NONE) == nullptr && strcmp(xd_im_argv(XD_IM_FCITX5)[0], "fcitx5") == 0);
    CHECK(strcmp(xd_im_config_tool(XD_IM_IBUS), "ibus-setup") == 0);
    CHECK(xd_im_program_is_framework("ibus-wayland") && xd_im_program_is_framework("fcitx5") &&
          !xd_im_program_is_framework("fcitx5-configtool") && !xd_im_program_is_framework(""));

    /* XIM and Qt's module; never GTK_IM_MODULE (GTK uses text-input-v3). */
    char *saved[XD_IM_ENV_VARS], *marks[XD_IM_ENV_VARS];
    for (size_t i = 0; i < XD_IM_ENV_VARS; i++) {
        char *mark = g_strconcat("XEONDESKTOP_", xd_im_env_vars[i], nullptr);
        saved[i] = g_strdup(g_getenv(xd_im_env_vars[i]));
        marks[i] = g_strdup(g_getenv(mark));
        g_unsetenv(xd_im_env_vars[i]);
        g_unsetenv(mark);
        g_free(mark);
    }
    char *gtk_saved = g_strdup(g_getenv("GTK_IM_MODULE"));
    g_unsetenv("GTK_IM_MODULE");
    CHECK(!xd_im_env_apply(XD_IM_NONE) && g_getenv("XMODIFIERS") == nullptr);
    CHECK(xd_im_env_apply(XD_IM_FCITX5));
    CHECK(strcmp(g_getenv("XMODIFIERS"), "@im=fcitx") == 0);
    CHECK(strcmp(g_getenv("QT_IM_MODULE"), "fcitx") == 0);
    CHECK(strcmp(g_getenv("QT_IM_MODULES"), "wayland;fcitx") == 0);
    CHECK(g_getenv("GTK_IM_MODULE") == nullptr);
    CHECK(!xd_im_env_apply(XD_IM_FCITX5));
    /* The user's own value stays; ours follow the choice, and are emptied
     * (not unset) for none. */
    g_setenv("QT_IM_MODULE", "mine", TRUE);
    CHECK(xd_im_env_apply(XD_IM_IBUS));
    CHECK(strcmp(g_getenv("XMODIFIERS"), "@im=ibus") == 0);
    CHECK(strcmp(g_getenv("QT_IM_MODULE"), "mine") == 0);
    CHECK(xd_im_env_apply(XD_IM_NONE));
    CHECK(strcmp(g_getenv("XMODIFIERS"), "") == 0 && strcmp(g_getenv("QT_IM_MODULE"), "mine") == 0);
    for (size_t i = 0; i < XD_IM_ENV_VARS; i++) {
        char *mark = g_strconcat("XEONDESKTOP_", xd_im_env_vars[i], nullptr);
        env_restore(xd_im_env_vars[i], saved[i]);
        env_restore(mark, marks[i]);
        g_free(saved[i]);
        g_free(marks[i]);
        g_free(mark);
    }
    env_restore("GTK_IM_MODULE", gtk_saved);
    g_free(gtk_saved);

    /* Fcitx 5's autostart entry: not in XeonDesktop, still in others. */
    char *root = g_dir_make_tmp("xd-im-XXXXXX", nullptr);
    char *sys1 = g_build_filename(root, "etc1", nullptr);
    char *sys2 = g_build_filename(root, "etc2", nullptr);
    char *user = g_build_filename(root, "user", "autostart", nullptr);
    g_mkdir_with_parents(sys1, 0700);
    g_mkdir_with_parents(sys2, 0700);
    g_mkdir_with_parents(user, 0700);
    char *file_path = g_build_filename(sys1, "org.fcitx.Fcitx5.desktop", nullptr);
    CHECK(g_file_set_contents(file_path,
                              "[Desktop Entry]\nType=Application\nName=Fcitx 5\n"
                              "Exec=/usr/bin/fcitx5 -d\nNotShowIn=Weston;\n",
                              -1, nullptr));
    g_free(file_path);
    /* A later directory's entry of the same name doesn't count. */
    file_path = g_build_filename(sys2, "org.fcitx.Fcitx5.desktop", nullptr);
    CHECK(g_file_set_contents(
        file_path, "[Desktop Entry]\nType=Application\nName=Other\nExec=true\n", -1, nullptr));
    g_free(file_path);
    file_path = g_build_filename(sys1, "ibus.desktop", nullptr);
    CHECK(g_file_set_contents(file_path,
                              "[Desktop Entry]\nType=Application\nName=IBus\n"
                              "Exec=ibus start --type wayland\n",
                              -1, nullptr));
    g_free(file_path);
    file_path = g_build_filename(sys1, "other.desktop", nullptr);
    CHECK(g_file_set_contents(
        file_path, "[Desktop Entry]\nType=Application\nName=Other\nExec=foo\n", -1, nullptr));
    g_free(file_path);
    /* The user's own copy of IBus's is theirs. */
    file_path = g_build_filename(user, "ibus.desktop", nullptr);
    CHECK(g_file_set_contents(
        file_path, "[Desktop Entry]\nType=Application\nName=Mine\nExec=ibus\n", -1, nullptr));
    g_free(file_path);
    const char *dirs[] = {sys1, sys2, nullptr};
    CHECK(xd_im_autostart_override(dirs, user, nullptr));
    CHECK(xd_im_autostart_override(dirs, user, nullptr)); /* again: ours, rewritten */
    GKeyFile *kf = g_key_file_new();
    file_path = g_build_filename(user, "org.fcitx.Fcitx5.desktop", nullptr);
    CHECK(g_key_file_load_from_file(kf, file_path, G_KEY_FILE_NONE, nullptr));
    char *not_in = g_key_file_get_string(kf, "Desktop Entry", "NotShowIn", nullptr);
    CHECK(not_in != nullptr && strcmp(not_in, "Weston;XeonDesktop;") == 0);
    char *exec = g_key_file_get_string(kf, "Desktop Entry", "Exec", nullptr);
    CHECK(exec != nullptr && strcmp(exec, "/usr/bin/fcitx5 -d") == 0);
    g_free(not_in);
    g_free(exec);
    g_free(file_path);
    g_key_file_free(kf);
    kf = g_key_file_new();
    file_path = g_build_filename(user, "ibus.desktop", nullptr);
    CHECK(g_key_file_load_from_file(kf, file_path, G_KEY_FILE_NONE, nullptr));
    char *name = g_key_file_get_string(kf, "Desktop Entry", "Name", nullptr);
    CHECK(name != nullptr && strcmp(name, "Mine") == 0);
    g_free(name);
    g_free(file_path);
    g_key_file_free(kf);
    file_path = g_build_filename(user, "other.desktop", nullptr);
    CHECK(!g_file_test(file_path, G_FILE_TEST_EXISTS));
    g_free(file_path);
    rm_tree(root);
    g_free(sys1);
    g_free(sys2);
    g_free(user);
    g_free(root);

    /* The candidate window: under the caret; above it at the bottom of
     * the screen; moved left at the right edge, never past the left. */
    xd_ime_rect screen = {1024, 0, 800, 600}; /* a second monitor */
    xd_ime_rect ime_rect = xd_ime_popup_place((xd_ime_rect){1100, 100, 1, 16}, 120, 30, screen);
    CHECK(ime_rect.x == 1100 && ime_rect.y == 116 && ime_rect.width == 120 &&
          ime_rect.height == 30);
    ime_rect = xd_ime_popup_place((xd_ime_rect){1100, 570, 1, 16}, 120, 30, screen);
    CHECK(ime_rect.x == 1100 && ime_rect.y == 540);
    ime_rect = xd_ime_popup_place((xd_ime_rect){1800, 100, 1, 16}, 120, 30, screen);
    CHECK(ime_rect.x == 1824 - 120 && ime_rect.y == 116);
    ime_rect = xd_ime_popup_place((xd_ime_rect){1030, 100, 1, 16}, 900, 30, screen);
    CHECK(ime_rect.x == 1024);
    /* Too tall for either side: stays below. */
    ime_rect = xd_ime_popup_place((xd_ime_rect){1100, 10, 1, 16}, 120, 590, screen);
    CHECK(ime_rect.y == 26);
}

/* A program taking the shortcuts (constraints.c): which keys stay ours, and
 * Ctrl+Alt on its own giving the mouse and keyboard back. */
static bool chord(xd_release_chord *release_chord, uint32_t sym, bool pressed) {
    return xd_release_chord_key(release_chord, &sym, 1, pressed);
}

static void test_grab_keys(void) {
    xd_grab_mods none = {0}, ctrl_alt = {.ctrl = true, .alt = true},
                 alt = {.alt = true}, logo = {.logo = true}, ctrl = {.ctrl = true};
    CHECK(xd_grab_key_kept(ctrl_alt, XKB_KEY_Delete));
    CHECK(xd_grab_key_kept(ctrl_alt, XKB_KEY_KP_Delete));
    CHECK(xd_grab_key_kept(ctrl_alt, XKB_KEY_BackSpace));
    CHECK(xd_grab_key_kept(ctrl_alt, XKB_KEY_XF86Switch_VT_1));
    CHECK(xd_grab_key_kept(ctrl_alt, XKB_KEY_XF86Switch_VT_12));
    CHECK(xd_grab_key_kept(logo, XKB_KEY_l));
    CHECK(xd_grab_key_kept(logo, XKB_KEY_L));
    /* The program's: Alt+Tab, Alt+F4, the Windows key and its combinations,
     * Ctrl+Esc, Ctrl+Shift+Esc, Ctrl+Alt+T, Delete without Ctrl+Alt. */
    CHECK(!xd_grab_key_kept(alt, XKB_KEY_Tab));
    CHECK(!xd_grab_key_kept(alt, XKB_KEY_F4));
    CHECK(!xd_grab_key_kept(none, XKB_KEY_Super_L));
    CHECK(!xd_grab_key_kept(logo, XKB_KEY_Up));
    CHECK(!xd_grab_key_kept(logo, XKB_KEY_d));
    CHECK(!xd_grab_key_kept(ctrl, XKB_KEY_Escape));
    CHECK(!xd_grab_key_kept((xd_grab_mods){.ctrl = true, .shift = true}, XKB_KEY_Escape));
    CHECK(!xd_grab_key_kept(ctrl_alt, XKB_KEY_t));
    CHECK(!xd_grab_key_kept(ctrl, XKB_KEY_Delete));
    CHECK(!xd_grab_key_kept(ctrl_alt, XKB_KEY_F1)); /* a keymap without VT keys */
    CHECK(!xd_grab_key_kept((xd_grab_mods){.logo = true, .ctrl = true}, XKB_KEY_l));

    /* Ctrl, Alt, let go: fires once, on the first release, either order. */
    xd_release_chord release_chord = {0};
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, true));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_L, true));
    CHECK(chord(&release_chord, XKB_KEY_Alt_L, false));
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, false));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_R, true));
    CHECK(!chord(&release_chord, XKB_KEY_Control_R, true));
    CHECK(chord(&release_chord, XKB_KEY_Control_R, false));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_R, false));
    /* Ctrl+Alt+Delete, Ctrl+Alt+T: a chord of their own. */
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, true));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_L, true));
    CHECK(!chord(&release_chord, XKB_KEY_Delete, true));
    CHECK(!chord(&release_chord, XKB_KEY_Delete, false));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_L, false));
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, false));
    /* Ctrl alone, Alt+Tab: nothing. */
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, true));
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, false));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_L, true));
    CHECK(!chord(&release_chord, XKB_KEY_Tab, true));
    CHECK(!chord(&release_chord, XKB_KEY_Tab, false));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_L, false));
    /* A key held from before (a game's W) doesn't spoil a fresh chord. */
    CHECK(!chord(&release_chord, XKB_KEY_w, true));
    CHECK(!chord(&release_chord, XKB_KEY_Control_L, true));
    CHECK(!chord(&release_chord, XKB_KEY_Alt_L, true));
    CHECK(chord(&release_chord, XKB_KEY_Control_L, false));
}

/* Passwords and Keys: texts and parsing, then OpenSSH's tools and GnuPG on
 * scratch directories (never ~/.ssh, ~/.gnupg or the user's agents). */
static void test_keys_texts(void) {
    int bits = 0;
    char *fp = nullptr, *comment = nullptr, *type = nullptr;
    CHECK(xd_ssh_parse_fingerprint("256 SHA256:pnWloDphx2AYPb8i0CarZphDkkSXZczXrHWFfSOLFI0 "
                                   "user@host (ED25519)\n",
                                   &bits, &fp, &comment, &type));
    CHECK(bits == 256);
    CHECK(g_strcmp0(fp, "SHA256:pnWloDphx2AYPb8i0CarZphDkkSXZczXrHWFfSOLFI0") == 0);
    CHECK(g_strcmp0(comment, "user@host") == 0);
    CHECK(g_strcmp0(type, "ED25519") == 0);
    g_free(fp);
    g_free(comment);
    g_free(type);
    /* A comment with spaces and brackets of its own. */
    CHECK(xd_ssh_parse_fingerprint("4096 SHA256:abc my work key (old) (RSA)", &bits, nullptr,
                                   &comment, &type));
    CHECK(bits == 4096 && g_strcmp0(comment, "my work key (old)") == 0 &&
          g_strcmp0(type, "RSA") == 0);
    g_free(comment);
    g_free(type);
    CHECK(!xd_ssh_parse_fingerprint("The agent has no identities.", nullptr, nullptr, nullptr,
                                    nullptr));
    CHECK(!xd_ssh_parse_fingerprint("", nullptr, nullptr, nullptr, nullptr));
    CHECK(!xd_ssh_parse_fingerprint("abc SHA256:x c (RSA)", nullptr, nullptr, nullptr, nullptr));

    CHECK(g_str_equal(xd_secret_kind("org.gnome.keyring.NetworkPassword"), "Network password"));
    CHECK(g_str_equal(xd_secret_kind("org.gnome.GVfs.Luks.Password"),
                      "Encrypted volume password"));
    CHECK(g_str_equal(xd_secret_kind(nullptr), "Password"));
    CHECK(g_str_equal(xd_secret_kind("com.example.Unknown"), "Password"));
    CHECK(g_str_equal(xd_secret_display_name("Mail", nullptr), "Mail"));
    CHECK(g_str_equal(xd_secret_display_name("", "org.gnome.keyring.Note"), "Stored note"));

    char *uid = xd_gpg_user_id("Ann Smith", "ann@example.com", "work");
    CHECK(g_str_equal(uid, "Ann Smith (work) <ann@example.com>"));
    g_free(uid);
    uid = xd_gpg_user_id("", "ann@example.com", "");
    CHECK(g_str_equal(uid, "<ann@example.com>"));
    g_free(uid);
    uid = xd_gpg_user_id("Ann", nullptr, nullptr);
    CHECK(g_str_equal(uid, "Ann"));
    g_free(uid);
    CHECK(g_str_equal(xd_gpg_validity_text(5), "Ultimate"));
    CHECK(g_str_equal(xd_gpg_validity_text(0), "Unknown"));
    CHECK(g_str_equal(xd_gpg_validity_text(2), "Never"));

    CHECK(xd_ssh_valid_key_name("id_ed25519"));
    CHECK(xd_ssh_valid_key_name("work key"));
    CHECK(!xd_ssh_valid_key_name(""));
    CHECK(!xd_ssh_valid_key_name("../id"));
    CHECK(!xd_ssh_valid_key_name(".hidden"));
    CHECK(!xd_ssh_valid_key_name("id.pub"));
    CHECK(!xd_ssh_valid_key_name("authorized_keys"));
    CHECK(!xd_ssh_valid_key_name("config"));

    char *message = xd_ssh_error_text("Load key \"/home/u/.ssh/id\": incorrect passphrase supplied "
                                      "to decrypt private key\n");
    CHECK(g_str_equal(message, "Incorrect passphrase supplied to decrypt private key."));
    g_free(message);
    message = xd_ssh_error_text("\n  \n");
    CHECK(g_str_equal(message, "The program failed without saying why."));
    g_free(message);

    char **argv = xd_ssh_keygen_argv("rsa", 4096, "me@here", "/x/id_rsa", true);
    const char *want[] = {"ssh-keygen", "-q", "-t", "rsa", "-b", "4096", "-C", "me@here",
                          "-f", "/x/id_rsa", "-N", "", nullptr};
    CHECK(g_strv_equal((const char *const *)argv, want));
    g_strfreev(argv);
    argv = xd_ssh_keygen_argv("ed25519", 0, "c", "/x/id", false);
    /* The passphrase is never on the command line. */
    CHECK(!g_strv_contains((const char *const *)argv, "-N") &&
          !g_strv_contains((const char *const *)argv, "-b"));
    g_strfreev(argv);
    argv = xd_ssh_add_argv("/x/id.pub", true);
    const char *want_add[] = {"ssh-add", "-d", "/x/id.pub", nullptr};
    CHECK(g_strv_equal((const char *const *)argv, want_add));
    g_strfreev(argv);
    char *answers = xd_ssh_answers((const char *const[]){"old", "new", "new", nullptr});
    CHECK(g_str_equal(answers, "old\nnew\nnew\n"));
    g_free(answers);
    answers = xd_ssh_answers(nullptr);
    CHECK(g_str_equal(answers, ""));
    g_free(answers);

    char *date = xd_keys_date(0);
    CHECK(g_str_equal(date, ""));
    g_free(date);
    date = xd_keys_day(86400 * 365);
    CHECK(date[0] != '\0');
    g_free(date);
}

/* Ends what a test started in a scratch GnuPG home: gpg-agent (and
 * scdaemon), found by that home on their command lines, since gpg-agent
 * keeps its environment unreadable. Then its socket directory. */
static void stop_scratch_gpg(const char *home) {
    GDir *proc = g_dir_open("/proc", 0, nullptr);
    char *arg = g_strdup_printf("--homedir%c%s", '\0', home);
    size_t arg_len = strlen("--homedir") + 1 + strlen(home);
    for (const char *entry_name;
         proc != nullptr && (entry_name = g_dir_read_name(proc)) != nullptr;) {
        if (!g_ascii_isdigit(entry_name[0]))
            continue;
        char *path = g_strdup_printf("/proc/%s/cmdline", entry_name);
        char *cmd = nullptr;
        gsize len = 0;
        if (g_file_get_contents(path, &cmd, &len, nullptr) &&
            memmem(cmd, len, arg, arg_len) != nullptr) {
            pid_t pid = (pid_t)atoi(entry_name);
            struct stat st;
            char *dir = g_strdup_printf("/proc/%s", entry_name);
            if (pid > 1 && stat(dir, &st) == 0 && st.st_uid == getuid())
                kill(pid, SIGTERM);
            g_free(dir);
        }
        g_free(cmd);
        g_free(path);
    }
    if (proc != nullptr)
        g_dir_close(proc);
    g_free(arg);
    /* gpg-agent removes its sockets as it ends. */
    for (int i = 0; i < 50; i++) {
        char *out = nullptr;
        const char *dirs[] = {"gpgconf", "--homedir", home, "--list-dirs", "agent-socket", nullptr};
        g_spawn_sync(nullptr, (char **)(uintptr_t)dirs, nullptr,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, nullptr, nullptr, &out,
                     nullptr, nullptr, nullptr);
        bool gone = out == nullptr || !g_file_test(g_strstrip(out), G_FILE_TEST_EXISTS);
        g_free(out);
        if (gone)
            break;
        g_usleep(100000);
    }
    const char *rm[] = {"gpgconf", "--homedir", home, "--remove-socketdir", nullptr};
    g_spawn_sync(nullptr, (char **)(uintptr_t)rm, nullptr,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDOUT_TO_DEV_NULL,
                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
}

static void test_keys_ssh(void) {
    char *keygen = g_find_program_in_path("ssh-keygen");
    char *agent_prog = g_find_program_in_path("ssh-agent");
    bool have = keygen != nullptr && agent_prog != nullptr;
    g_free(keygen);
    g_free(agent_prog);
    if (!have) {
        fprintf(stderr, "test_keys_ssh: skipped, OpenSSH isn't installed\n");
        return;
    }
    char *dir = g_dir_make_tmp("xd-keys-ssh-XXXXXX", nullptr);
    CHECK(dir != nullptr);
    if (dir == nullptr)
        return;
    /* xd-tests is its own askpass (main()). */
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *path = g_build_filename(dir, "id_ed25519", nullptr);

    char *name = xd_ssh_new_key_name(dir, "ed25519");
    CHECK(g_str_equal(name, "id_ed25519"));
    g_free(name);

    /* A key with a passphrase: asked twice, answered from the pipe. */
    char **argv = xd_ssh_keygen_argv("ed25519", 0, "test key", path, false);
    GError *error = nullptr;
    CHECK(xd_ssh_run((const char *const *)argv, self, (const char *const[]){"pw1", "pw1", nullptr},
                     &error));
    g_clear_error(&error);
    g_strfreev(argv);
    name = xd_ssh_new_key_name(dir, "ed25519");
    CHECK(g_str_equal(name, "id_ed25519_2"));
    g_free(name);

    GPtrArray *keys = xd_ssh_list_keys(dir);
    CHECK(keys->len == 1);
    if (keys->len == 1) {
        const xd_ssh_key *ssh_key = keys->pdata[0];
        CHECK(g_str_equal(ssh_key->name, "id_ed25519"));
        CHECK(g_str_equal(ssh_key->type, "ED25519") && ssh_key->bits == 256);
        CHECK(g_str_equal(ssh_key->comment, "test key"));
        CHECK(g_str_has_prefix(ssh_key->fingerprint, "SHA256:"));
        CHECK(ssh_key->has_private && ssh_key->encrypted);
    }

    /* A wrong old passphrase is refused, and says so. */
    argv = xd_ssh_passwd_argv(path, false);
    CHECK(!xd_ssh_run((const char *const *)argv, self,
                      (const char *const[]){"wrong", "pw2", "pw2", nullptr}, &error));
    CHECK(error != nullptr && strstr(error->message, "passphrase") != nullptr);
    g_clear_error(&error);
    /* The right one changes it; then none at all. */
    CHECK(xd_ssh_run((const char *const *)argv, self,
                     (const char *const[]){"pw1", "pw2", "pw2", nullptr}, &error));
    g_clear_error(&error);
    g_strfreev(argv);
    argv = xd_ssh_passwd_argv(path, true);
    CHECK(xd_ssh_run((const char *const *)argv, self, (const char *const[]){"pw2", nullptr},
                     &error));
    g_clear_error(&error);
    g_strfreev(argv);
    CHECK(!xd_ssh_key_encrypted(path));
    argv = xd_ssh_passwd_argv(path, false);
    CHECK(xd_ssh_run((const char *const *)argv, self, (const char *const[]){"pw3", "pw3", nullptr},
                     &error));
    g_clear_error(&error);
    g_strfreev(argv);
    CHECK(xd_ssh_key_encrypted(path));

    /* An agent of our own on a scratch socket, never the user's. */
    char *sock = g_build_filename(dir, "agent.sock", nullptr);
    char *saved = g_strdup(g_getenv("SSH_AUTH_SOCK"));
    g_unsetenv("SSH_AUTH_SOCK");
    char **none = xd_ssh_agent_fingerprints(&error);
    CHECK(none == nullptr && error != nullptr);
    g_clear_error(&error);
    const char *agent_argv[] = {"ssh-agent", "-D", "-a", sock, nullptr};
    GPid agent = 0;
    bool started = g_spawn_async(nullptr, (char **)(uintptr_t)agent_argv, nullptr,
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                     G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                 nullptr, nullptr, &agent, nullptr);
    CHECK(started);
    for (int i = 0; started && i < 50 && !g_file_test(sock, G_FILE_TEST_EXISTS); i++)
        g_usleep(100000);
    g_setenv("SSH_AUTH_SOCK", sock, TRUE);
    char **fps = xd_ssh_agent_fingerprints(&error);
    CHECK(fps != nullptr && g_strv_length(fps) == 0);
    g_strfreev(fps);
    g_clear_error(&error);
    char **add = xd_ssh_add_argv(path, false);
    /* A wrong passphrase: askpass has no second answer, so ssh-add gives up. */
    CHECK(!xd_ssh_run((const char *const *)add, self, (const char *const[]){"nope", nullptr},
                      &error));
    g_clear_error(&error);
    CHECK(xd_ssh_run((const char *const *)add, self, (const char *const[]){"pw3", nullptr},
                     &error));
    g_clear_error(&error);
    g_strfreev(add);
    fps = xd_ssh_agent_fingerprints(&error);
    CHECK(fps != nullptr && g_strv_length(fps) == 1);
    xd_ssh_mark_agent_keys(keys, fps);
    CHECK(keys->len == 1 && ((xd_ssh_key *)keys->pdata[0])->in_agent);
    g_strfreev(fps);
    char *pub = g_strconcat(path, ".pub", nullptr);
    add = xd_ssh_add_argv(pub, true);
    CHECK(xd_ssh_run((const char *const *)add, self, nullptr, &error));
    g_clear_error(&error);
    g_strfreev(add);
    fps = xd_ssh_agent_fingerprints(&error);
    CHECK(fps != nullptr && g_strv_length(fps) == 0);
    g_strfreev(fps);
    if (started) {
        kill(agent, SIGTERM);
        waitpid(agent, nullptr, 0);
        g_spawn_close_pid(agent);
    }
    if (saved != nullptr)
        g_setenv("SSH_AUTH_SOCK", saved, TRUE);
    else
        g_unsetenv("SSH_AUTH_SOCK");
    g_free(saved);
    g_ptr_array_unref(keys);

    /* A public key alone is listed, without a private half. */
    CHECK(g_remove(path) == 0);
    keys = xd_ssh_list_keys(dir);
    CHECK(keys->len == 1 && !((xd_ssh_key *)keys->pdata[0])->has_private);
    g_ptr_array_unref(keys);
    g_remove(sock);
    g_free(sock);
    g_free(pub);
    g_free(path);
    g_free(self);
    rm_tree(dir);
    g_free(dir);
}

static char *no_passphrase([[maybe_unused]] void *data, [[maybe_unused]] const char *hint,
                           [[maybe_unused]] bool again) {
    return nullptr;
}

static void test_keys_gpg(void) {
    char *gpg = g_find_program_in_path("gpg");
    g_free(gpg);
    if (gpg == nullptr) {
        fprintf(stderr, "test_keys_gpg: skipped, GnuPG isn't installed\n");
        return;
    }
    char *home = g_dir_make_tmp("xd-keys-gpg-XXXXXX", nullptr);
    CHECK(home != nullptr);
    if (home == nullptr)
        return;
    /* No smartcard daemon for a scratch keyring. */
    char *conf = g_build_filename(home, "gpg-agent.conf", nullptr);
    CHECK(g_file_set_contents(conf, "disable-scdaemon\n", -1, nullptr));
    g_free(conf);
    GError *error = nullptr;
    xd_gpg *keyring = xd_gpg_new(home, no_passphrase, nullptr, &error);
    CHECK(keyring != nullptr);
    g_clear_error(&error);
    if (keyring == nullptr) {
        rm_tree(home);
        g_free(home);
        return;
    }
    GPtrArray *keys = xd_gpg_list(keyring, &error);
    CHECK(keys != nullptr && keys->len == 0);
    if (keys != nullptr)
        g_ptr_array_unref(keys);
    g_clear_error(&error);

    char *uid = xd_gpg_user_id("Test User", "test@example.com", "unit test");
    char *fpr = nullptr;
    CHECK(xd_gpg_create(keyring, uid, "", 0, &fpr, &error));
    g_clear_error(&error);
    g_free(uid);
    CHECK(fpr != nullptr && strlen(fpr) == 40);
    keys = xd_gpg_list(keyring, &error);
    CHECK(keys != nullptr && keys->len == 1);
    if (keys != nullptr && keys->len == 1) {
        const xd_gpg_key *gpg_key = keys->pdata[0];
        CHECK(g_str_equal(gpg_key->name, "Test User"));
        CHECK(g_str_equal(gpg_key->email, "test@example.com"));
        CHECK(g_str_equal(gpg_key->comment, "unit test"));
        CHECK(g_strcmp0(gpg_key->fingerprint, fpr) == 0);
        CHECK(g_str_has_suffix(fpr, gpg_key->key_id));
        CHECK(gpg_key->secret && gpg_key->expires == 0 && gpg_key->created > 0);
        CHECK(gpg_key->validity == 5); /* our own: ultimate */
        CHECK(g_strcmp0(gpg_key->algorithm, "ed25519") == 0);
        CHECK(gpg_key->subkeys == 2); /* and Curve25519 for encryption */
    }
    if (keys != nullptr)
        g_ptr_array_unref(keys);

    /* A year from now; the key has no passphrase, so nothing is asked. */
    CHECK(xd_gpg_set_expiry(keyring, fpr, 365ULL * 86400, &error));
    g_clear_error(&error);
    keys = xd_gpg_list(keyring, &error);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    CHECK(keys != nullptr && keys->len == 1 &&
          ((xd_gpg_key *)keys->pdata[0])->expires > now + 364 * 86400);
    if (keys != nullptr)
        g_ptr_array_unref(keys);

    char *armour = xd_gpg_export(keyring, fpr, &error);
    CHECK(armour != nullptr && strstr(armour, "BEGIN PGP PUBLIC KEY BLOCK") != nullptr &&
          strstr(armour, "PRIVATE") == nullptr);
    g_clear_error(&error);
    char *file = g_build_filename(home, "exported.asc", nullptr);
    CHECK(armour != nullptr && g_file_set_contents(file, armour, -1, nullptr));
    g_free(armour);

    CHECK(xd_gpg_delete(keyring, fpr, &error));
    g_clear_error(&error);
    keys = xd_gpg_list(keyring, &error);
    CHECK(keys != nullptr && keys->len == 0);
    if (keys != nullptr)
        g_ptr_array_unref(keys);

    /* Back as a public key alone. */
    int imported = 0, unchanged = 0;
    CHECK(xd_gpg_import_file(keyring, file, &imported, &unchanged, &error));
    g_clear_error(&error);
    CHECK(imported == 1 && unchanged == 0);
    CHECK(xd_gpg_import_file(keyring, file, &imported, &unchanged, &error));
    g_clear_error(&error);
    CHECK(imported == 0 && unchanged == 1);
    keys = xd_gpg_list(keyring, &error);
    CHECK(keys != nullptr && keys->len == 1 && !((xd_gpg_key *)keys->pdata[0])->secret);
    if (keys != nullptr)
        g_ptr_array_unref(keys);
    /* Expiry needs the secret key. */
    CHECK(!xd_gpg_set_expiry(keyring, fpr, 0, &error));
    g_clear_error(&error);
    /* Not a key file. */
    CHECK(g_file_set_contents(file, "hello\n", -1, nullptr));
    CHECK(!xd_gpg_import_file(keyring, file, &imported, &unchanged, &error));
    CHECK(error != nullptr);
    g_clear_error(&error);
    g_free(file);
    g_free(fpr);
    xd_gpg_free(keyring);
    stop_scratch_gpg(home);
    rm_tree(home);
    g_free(home);
}

/* Map Network Drive: folders as typed, the remembered list, and the
 * answers given to gvfs's questions. */
static gboolean net_parses(const char *text, gboolean need_share, const char *want) {
    GError *err = nullptr;
    char *uri = xd_net_parse_folder(text, need_share, &err);
    gboolean ok = want == nullptr ? uri == nullptr && err != nullptr
                                  : uri != nullptr && strcmp(uri, want) == 0 && err == nullptr;
    if (!ok)
        fprintf(stderr, "  parse %s: %s\n", text, uri != nullptr ? uri : err->message);
    g_free(uri);
    g_clear_error(&err);
    return ok;
}

static GMountOperationResult net_reply_result;
static int net_replies;

static void net_on_reply([[maybe_unused]] GMountOperation *op, GMountOperationResult result,
                         [[maybe_unused]] gpointer data) {
    net_reply_result = result;
    net_replies++;
}

static void test_network_drives(void) {
    CHECK(net_parses("\\\\server\\share", TRUE, "smb://server/share"));
    CHECK(net_parses("  \\\\server\\my share\\sub dir\\ ", TRUE,
                     "smb://server/my%20share/sub%20dir"));
    CHECK(net_parses("\\\\server/share/", TRUE, "smb://server/share"));
    CHECK(net_parses("\\\\server", TRUE, nullptr));
    CHECK(net_parses("\\\\server", FALSE, "smb://server/"));
    CHECK(net_parses("\\\\", FALSE, nullptr));
    CHECK(net_parses("nfs://nas/export/home/", TRUE, "nfs://nas/export/home"));
    CHECK(net_parses("SMB://Server/Share", TRUE, "smb://Server/Share"));
    CHECK(net_parses("sftp://me@host:2222/srv/", TRUE, "sftp://me@host:2222/srv"));
    CHECK(net_parses("smb:///", FALSE, nullptr));
    /* Kinds without servers (gvfs's localtest), but nothing local. */
    CHECK(net_parses("localtest:///etc/", TRUE, "localtest:///etc"));
    CHECK(net_parses("localtest:///", TRUE, nullptr));
    CHECK(net_parses("file:///etc", TRUE, nullptr));
    CHECK(net_parses("trash:///x", TRUE, nullptr));
    CHECK(net_parses("server\\share", TRUE, nullptr));
    CHECK(net_parses("", FALSE, nullptr));

    CHECK(xd_net_same_uri("smb://SERVER/Share/", "smb://server/share"));
    CHECK(xd_net_same_uri("smb://s/my%20share", "smb://s/My%20Share"));
    CHECK(!xd_net_same_uri("nfs://nas/Export", "nfs://nas/export"));
    CHECK(!xd_net_same_uri("smb://a/share", "smb://b/share"));
    CHECK(!xd_net_same_uri("smb://a/share", "nfs://a/share"));

    char *display_path = xd_net_display_path("smb://server/my%20share/sub");
    CHECK(strcmp(display_path, "\\\\server\\my share\\sub") == 0);
    g_free(display_path);
    display_path = xd_net_display_path("smb://server/");
    CHECK(strcmp(display_path, "\\\\server") == 0);
    g_free(display_path);
    display_path = xd_net_display_path("nfs://nas/export%20x");
    CHECK(strcmp(display_path, "nfs://nas/export x") == 0);
    g_free(display_path);
    display_path = xd_net_drive_name("smb://server/my%20share");
    CHECK(strcmp(display_path, "my share on 'server'") == 0);
    g_free(display_path);
    display_path = xd_net_drive_name("smb://server/");
    CHECK(strcmp(display_path, "server") == 0);
    g_free(display_path);
    display_path = xd_net_drive_name("localtest:///etc");
    CHECK(strcmp(display_path, "etc") == 0);
    g_free(display_path);
    display_path = xd_net_host("nfs://nas.local/export");
    CHECK(g_strcmp0(display_path, "nas.local") == 0);
    g_free(display_path);
    display_path = xd_net_unsupported_text("smb://server/share");
    CHECK(strstr(display_path, "gvfs-smb") != nullptr);
    g_free(display_path);
    display_path = xd_net_unsupported_text("nfs://server/path");
    CHECK(strstr(display_path, "gvfs-nfs") != nullptr);
    g_free(display_path);

    /* The list: drives and places, unknown groups kept. */
    char *dir = g_dir_make_tmp("xd-network-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "xeondesktop", "network.ini", nullptr);
    GPtrArray *none = xd_net_drives_load(path);
    CHECK(none->len == 0);
    g_ptr_array_unref(none);
    CHECK(xd_net_drive_add(path, "smb://server/share", nullptr, TRUE, nullptr));
    CHECK(xd_net_drive_add(path, "nfs://nas/export", "OFFICE\\bob", FALSE, nullptr));
    CHECK(xd_net_place_add(path, "sftp://example.org/srv", "Web files", nullptr));
    char *text = nullptr;
    CHECK(g_file_get_contents(path, &text, nullptr, nullptr));
    char *more = g_strconcat(text, "\n[Future]\nKey=1\n", nullptr);
    CHECK(g_file_set_contents(path, more, -1, nullptr));
    g_free(more);
    g_free(text);
    /* The same folder written differently: replaced, not added. */
    CHECK(xd_net_drive_add(path, "smb://SERVER/share/", "alice", TRUE, nullptr));
    GPtrArray *drives = xd_net_drives_load(path);
    CHECK(drives->len == 2);
    if (drives->len == 2) {
        XdNetDrive *first = g_ptr_array_index(drives, 0), *second = g_ptr_array_index(drives, 1);
        CHECK(strcmp(first->uri, "smb://SERVER/share/") == 0 && first->reconnect &&
              g_strcmp0(first->user, "alice") == 0);
        CHECK(strcmp(second->uri, "nfs://nas/export") == 0 && !second->reconnect &&
              g_strcmp0(second->user, "OFFICE\\bob") == 0);
    }
    g_ptr_array_unref(drives);
    GPtrArray *places = xd_net_places_load(path);
    CHECK(places->len == 1 &&
          strcmp(((XdNetPlace *)g_ptr_array_index(places, 0))->name, "Web files") == 0);
    g_ptr_array_unref(places);
    /* Logon forgets the drive mapped without Reconnect. */
    CHECK(xd_net_forget_session_drives(path, nullptr));
    drives = xd_net_drives_load(path);
    CHECK(drives->len == 1);
    g_ptr_array_unref(drives);
    CHECK(xd_net_drive_remove(path, "smb://server/share", nullptr));
    CHECK(xd_net_place_remove(path, "sftp://example.org/srv/", nullptr));
    CHECK(xd_net_drive_remove(path, "smb://nowhere/x", nullptr));
    drives = xd_net_drives_load(path);
    places = xd_net_places_load(path);
    CHECK(drives->len == 0 && places->len == 0);
    g_ptr_array_unref(drives);
    g_ptr_array_unref(places);
    CHECK(g_file_get_contents(path, &text, nullptr, nullptr));
    CHECK(strstr(text, "[Future]") != nullptr);
    g_free(text);
    char *sub = g_path_get_dirname(path);
    CHECK(g_remove(path) == 0);
    CHECK(g_rmdir(sub) == 0);
    CHECK(g_rmdir(dir) == 0);
    g_free(sub);
    g_free(path);
    g_free(dir);

    /* "Connect to server" answers: DOMAIN\user split, the password kept
     * by the Secret Service only when asked and possible. */
    GMountOperation *op = g_mount_operation_new();
    g_signal_connect(op, "reply", G_CALLBACK(net_on_reply), nullptr);
    GAskPasswordFlags all = (GAskPasswordFlags)(G_ASK_PASSWORD_NEED_USERNAME |
                                                G_ASK_PASSWORD_NEED_PASSWORD |
                                                G_ASK_PASSWORD_NEED_DOMAIN |
                                                G_ASK_PASSWORD_SAVING_SUPPORTED);
    net_replies = 0;
    xd_net_login_reply(op, all, "OFFICE\\bob", "pw", TRUE);
    CHECK(net_replies == 1 && net_reply_result == G_MOUNT_OPERATION_HANDLED);
    CHECK(g_strcmp0(g_mount_operation_get_username(op), "bob") == 0);
    CHECK(g_strcmp0(g_mount_operation_get_domain(op), "OFFICE") == 0);
    CHECK(g_strcmp0(g_mount_operation_get_password(op), "pw") == 0);
    CHECK(g_mount_operation_get_password_save(op) == G_PASSWORD_SAVE_PERMANENTLY);
    CHECK(!g_mount_operation_get_anonymous(op));
    xd_net_login_reply(op, (GAskPasswordFlags)(G_ASK_PASSWORD_NEED_USERNAME |
                                               G_ASK_PASSWORD_NEED_PASSWORD),
                       "OFFICE\\carol", "x", TRUE);
    CHECK(g_strcmp0(g_mount_operation_get_username(op), "OFFICE\\carol") == 0);
    CHECK(g_mount_operation_get_password_save(op) == G_PASSWORD_SAVE_NEVER);
    xd_net_login_reply(op, (GAskPasswordFlags)(all | G_ASK_PASSWORD_ANONYMOUS_SUPPORTED), "",
                       "", FALSE);
    CHECK(g_mount_operation_get_anonymous(op));
    g_object_unref(op);

    /* Connect As answers the first question by itself, not the next. */
    op = g_mount_operation_new();
    g_signal_connect(op, "reply", G_CALLBACK(net_on_reply), nullptr);
    xd_net_operation_preset(op, "alice", "secret");
    net_replies = 0;
    CHECK(xd_net_answer_preset(op, all));
    CHECK(net_replies == 1 && g_strcmp0(g_mount_operation_get_username(op), "alice") == 0 &&
          g_strcmp0(g_mount_operation_get_password(op), "secret") == 0);
    CHECK(!xd_net_answer_preset(op, all));
    CHECK(net_replies == 1);
    g_object_unref(op);

    /* The logon one answers nothing: one reply, Aborted, and not the
     * default handler's "unhandled" after it. */
    op = xd_net_quiet_operation_new();
    g_signal_connect(op, "reply", G_CALLBACK(net_on_reply), nullptr);
    net_replies = 0;
    g_signal_emit_by_name(op, "ask-password", "Password required", "me", "WORKGROUP", all);
    for (int i = 0; i < 20; i++)
        g_main_context_iteration(nullptr, FALSE);
    CHECK(net_replies == 1 && net_reply_result == G_MOUNT_OPERATION_ABORTED);
    const char *choices[] = {"Yes", "No", nullptr};
    g_signal_emit_by_name(op, "ask-question", "Trust this key?", choices);
    for (int i = 0; i < 20; i++)
        g_main_context_iteration(nullptr, FALSE);
    CHECK(net_replies == 2 && net_reply_result == G_MOUNT_OPERATION_ABORTED);
    g_object_unref(op);
}

/* Sharing: `net usershare` against a scratch smb.conf (XD_SMB_CONF), never
 * the system's. */
static char *usershare_conf(const char *dir, const char *name, const char *body) {
    char *conf = g_build_filename(dir, name, nullptr);
    CHECK(g_file_set_contents(conf, body, -1, nullptr));
    return conf;
}

static void test_usershare(void) {
    static const char info[] = "[docs]\n"
                               "path=/home/me/My Docs\n"
                               "comment=\n"
                               "usershare_acl=S-1-1-0:F,\n"
                               "guest_ok=n\n"
                               "\n"
                               "[music]\n"
                               "path=/home/me/Music\n"
                               "comment=tunes\n"
                               "usershare_acl=Everyone:R,\n"
                               "guest_ok=y\n";
    GPtrArray *shares = xd_usershare_parse_info(info);
    CHECK(shares->len == 2);
    const XdUsershare *docs = xd_usershare_find(shares, "/home/me/My Docs");
    const XdUsershare *music = xd_usershare_find(shares, "/home/me/Music");
    CHECK(docs != nullptr && strcmp(docs->name, "docs") == 0 && docs->writable &&
          !docs->guest_ok);
    CHECK(music != nullptr && !music->writable && music->guest_ok &&
          strcmp(music->comment, "tunes") == 0);
    CHECK(xd_usershare_find(shares, "/home/me") == nullptr);
    g_ptr_array_unref(shares);

    CHECK(xd_usershare_status_of("net usershare: usershares are currently disabled") ==
          XD_USERSHARE_DISABLED);
    CHECK(xd_usershare_status_of("net usershare add: cannot create tmp file /var/lib/x/:tmp") ==
          XD_USERSHARE_NO_PERMISSION);
    CHECK(xd_usershare_status_of("something else") == XD_USERSHARE_FAILED);
    CHECK(xd_usershare_name_problem("") != nullptr);
    CHECK(xd_usershare_name_problem("Shared Docs") == nullptr);
    CHECK(xd_usershare_name_problem("a:b") != nullptr);
    CHECK(xd_usershare_name_problem("a%b") != nullptr);
    char *long_name = g_strnfill(81, 'x');
    CHECK(xd_usershare_name_problem(long_name) != nullptr);
    g_free(long_name);
    char *name = xd_usershare_default_name("/home/me/Work: 2026");
    CHECK(strcmp(name, "Work_ 2026") == 0);
    g_free(name);
    char *help = xd_usershare_help_text(XD_USERSHARE_DISABLED, nullptr);
    CHECK(strstr(help, "usershare path") != nullptr && strstr(help, "smb.conf") != nullptr);
    g_free(help);
    CHECK(xd_usershare_help_text(XD_USERSHARE_OK, nullptr) == nullptr);

    char *dir = g_dir_make_tmp("xd-usershare-XXXXXX", nullptr);
    char *store = g_build_filename(dir, "usershares", nullptr);
    char *ro = g_build_filename(dir, "readonly", nullptr);
    char *folder = g_build_filename(dir, "My Folder", nullptr);
    CHECK(g_mkdir(store, 01770) == 0 && g_mkdir(ro, 0555) == 0 && g_mkdir(folder, 0755) == 0);
    CHECK(xd_usershare_dir_writable(store, nullptr));
    char *group = nullptr;
    if (geteuid() != 0) {
        CHECK(!xd_usershare_dir_writable(ro, &group));
        CHECK(group != nullptr);
        help = xd_usershare_help_text(XD_USERSHARE_NO_PERMISSION, ro);
        CHECK(strstr(help, ro) != nullptr && group != nullptr && strstr(help, group) != nullptr);
        g_free(help);
    }
    g_free(group);

    char *net = g_find_program_in_path("net");
    char *testparm = g_find_program_in_path("testparm");
    if (net == nullptr) {
        printf("  (net not installed: user share runs skipped)\n");
    } else {
        char *saved = g_strdup(g_getenv("XD_SMB_CONF"));
        char *body = g_strdup_printf("[global]\n  usershare path = %s\n"
                                     "  usershare max shares = 10\n",
                                     store);
        char *on = usershare_conf(dir, "on.conf", body);
        g_free(body);
        g_setenv("XD_SMB_CONF", on, TRUE);
        XdUsershareStatus st;
        char *detail = nullptr;
        shares = xd_usershare_probe(&st, &detail);
        CHECK(st == XD_USERSHARE_OK && shares->len == 0);
        g_ptr_array_unref(shares);
        g_free(detail);
        GError *err = nullptr;
        CHECK(xd_usershare_add("xdtest", folder, "", TRUE, &err));
        if (err != nullptr)
            fprintf(stderr, "  usershare add: %s\n", err->message);
        g_clear_error(&err);
        shares = xd_usershare_probe(&st, nullptr);
        const XdUsershare *made = xd_usershare_find(shares, folder);
        CHECK(made != nullptr && strcmp(made->name, "xdtest") == 0 && made->writable &&
              !made->guest_ok);
        g_ptr_array_unref(shares);
        CHECK(xd_usershare_add("xdtest", folder, "", FALSE, nullptr));
        shares = xd_usershare_probe(&st, nullptr);
        made = xd_usershare_find(shares, folder);
        CHECK(made != nullptr && !made->writable);
        g_ptr_array_unref(shares);
        CHECK(xd_usershare_delete("xdtest", nullptr));
        shares = xd_usershare_probe(&st, nullptr);
        CHECK(shares->len == 0);
        g_ptr_array_unref(shares);
        char *share_file = g_build_filename(store, "xdtest", nullptr);
        CHECK(!g_file_test(share_file, G_FILE_TEST_EXISTS));
        g_free(share_file);

        /* Turned off: no usershare path. */
        char *off = usershare_conf(dir, "off.conf", "[global]\n  workgroup = TEST\n");
        g_setenv("XD_SMB_CONF", off, TRUE);
        shares = xd_usershare_probe(&st, &detail);
        CHECK(st == XD_USERSHARE_DISABLED);
        g_ptr_array_unref(shares);
        g_free(detail);
        CHECK(!xd_usershare_add("xdtest", folder, "", FALSE, &err));
        CHECK(err != nullptr && xd_usershare_status_of(err->message) == XD_USERSHARE_DISABLED);
        g_clear_error(&err);

        /* A folder we can't write: not in the group. */
        if (testparm != nullptr && geteuid() != 0) {
            body = g_strdup_printf("[global]\n  usershare path = %s\n"
                                   "  usershare max shares = 10\n",
                                   ro);
            char *denied = usershare_conf(dir, "denied.conf", body);
            g_free(body);
            g_setenv("XD_SMB_CONF", denied, TRUE);
            shares = xd_usershare_probe(&st, &detail);
            CHECK(st == XD_USERSHARE_NO_PERMISSION && g_strcmp0(detail, ro) == 0);
            g_ptr_array_unref(shares);
            g_free(detail);
            CHECK(g_remove(denied) == 0);
            g_free(denied);
        }
        if (saved != nullptr)
            g_setenv("XD_SMB_CONF", saved, TRUE);
        else
            g_unsetenv("XD_SMB_CONF");
        g_free(saved);
        CHECK(g_remove(on) == 0 && g_remove(off) == 0);
        g_free(on);
        g_free(off);
    }
    g_free(net);
    g_free(testparm);
    CHECK(g_rmdir(store) == 0 && g_rmdir(ro) == 0 && g_rmdir(folder) == 0 && g_rmdir(dir) == 0);
    g_free(store);
    g_free(ro);
    g_free(folder);
    g_free(dir);
}

/* Automatic Updates: [Updates], the schedule, the state the shell watches,
 * importance, and reading what fwupd says. */
static int64_t utc_at(int y, int mo, int day, int hour, int mi) {
    GDateTime *date_time = g_date_time_new_utc(y, mo, day, hour, mi, 0);
    int64_t seconds = g_date_time_to_unix(date_time);
    g_date_time_unref(date_time);
    return seconds;
}

static void test_updates(void) {
    xd_settings original;
    xd_settings_defaults(&original);
    CHECK(original.updates.mode == XD_UPDATES_DOWNLOAD && original.updates.day == 0 &&
          original.updates.hour == 3);
    xd_settings_parse(XD_STR("[Updates]\nMode=notify\nDay=friday\nHour=22\n"), &original);
    CHECK(original.updates.mode == XD_UPDATES_NOTIFY && original.updates.day == 6 &&
          original.updates.hour == 22);
    xd_settings_parse(XD_STR("[Updates]\nMode=always\nDay=someday\nHour=24\n"), &original);
    CHECK(original.updates.mode == XD_UPDATES_DOWNLOAD && original.updates.day == 0 &&
          original.updates.hour == 3);
    original.updates = (xd_update_settings){.mode = XD_UPDATES_OFF, .day = 7, .hour = 0};
    xd_strbuf out = {0};
    xd_settings_serialize(&original, &out);
    CHECK(strstr(xd_strbuf_cstr(&out), "Mode=off") != nullptr);
    CHECK(strstr(xd_strbuf_cstr(&out), "Day=saturday") != nullptr);
    xd_settings parsed;
    xd_settings_parse(xd_strbuf_view(&out), &parsed);
    CHECK(parsed.updates.mode == XD_UPDATES_OFF && parsed.updates.day == 7 &&
          parsed.updates.hour == 0);
    xd_strbuf_free(&out);
    CHECK(strcmp(xd_updates_mode_name(XD_UPDATES_NOTIFY), "notify") == 0);

    /* The schedule (UTC; 2026-10-18 is a Sunday). */
    GTimeZone *utc = g_time_zone_new_utc();
    xd_update_settings daily = {.mode = XD_UPDATES_DOWNLOAD, .day = 0, .hour = 3};
    CHECK(xd_updates_next_check(&daily, 0, utc) == 0); /* never checked: now */
    CHECK(xd_updates_next_check(&daily, utc_at(2026, 10, 18, 2, 0), utc) ==
          utc_at(2026, 10, 18, 3, 0));
    CHECK(xd_updates_next_check(&daily, utc_at(2026, 10, 18, 3, 0), utc) ==
          utc_at(2026, 10, 19, 3, 0));
    CHECK(xd_updates_next_check(&daily, utc_at(2026, 10, 18, 9, 30), utc) ==
          utc_at(2026, 10, 19, 3, 0));
    CHECK(xd_updates_next_check(&daily, utc_at(2026, 12, 31, 23, 0), utc) ==
          utc_at(2027, 1, 1, 3, 0));
    xd_update_settings sunday = {.mode = XD_UPDATES_NOTIFY, .day = 1, .hour = 15};
    CHECK(xd_updates_next_check(&sunday, utc_at(2026, 10, 18, 9, 0), utc) ==
          utc_at(2026, 10, 18, 15, 0));
    CHECK(xd_updates_next_check(&sunday, utc_at(2026, 10, 18, 16, 0), utc) ==
          utc_at(2026, 10, 25, 15, 0));
    xd_update_settings saturday = {.mode = XD_UPDATES_DOWNLOAD, .day = 7, .hour = 0};
    CHECK(xd_updates_next_check(&saturday, utc_at(2026, 10, 18, 0, 0), utc) ==
          utc_at(2026, 10, 24, 0, 0));
    /* A week missed (the computer was off): due at once. */
    CHECK(xd_updates_next_check(&daily, utc_at(2026, 10, 1, 12, 0), utc) <
          utc_at(2026, 10, 18, 0, 0));
    xd_update_settings off = {.mode = XD_UPDATES_OFF};
    CHECK(xd_updates_next_check(&off, 0, utc) == INT64_MAX);
    g_time_zone_unref(utc);

    /* The state file. */
    char *dir = g_dir_make_tmp("xd-updates-XXXXXX", nullptr);
    char *path = g_build_filename(dir, "updates.ini", nullptr);
    xd_updates_state st;
    CHECK(!xd_updates_state_read(path, &st) && st.checked == 0);
    const char *ids[] = {"b;2;x86_64;extra", "a;1;x86_64;core"};
    const char *rev[] = {ids[1], ids[0]};
    xd_updates_state updates_state = {
        .checked = 1760000000, .software = 5, .firmware = 1, .important = 2, .downloaded = true};
    xd_updates_signature(ids, 2, updates_state.signature);
    char sig2[41];
    xd_updates_signature(rev, 2, sig2);
    CHECK(strlen(updates_state.signature) == 40 && strcmp(updates_state.signature, sig2) == 0);
    xd_updates_signature(ids, 1, sig2);
    CHECK(strcmp(updates_state.signature, sig2) != 0);
    xd_updates_signature(ids, 0, sig2);
    CHECK(sig2[0] == '\0');
    g_strlcpy(updates_state.error, "Listing the updates: no network", sizeof(updates_state.error));
    CHECK(xd_updates_state_write(path, &updates_state));
    CHECK(xd_updates_state_read(path, &st));
    CHECK(st.checked == 1760000000 && st.software == 5 && st.firmware == 1 && st.important == 2);
    CHECK(st.downloaded && !st.metered && strcmp(st.signature, updates_state.signature) == 0);
    CHECK(strcmp(st.error, updates_state.error) == 0);
    CHECK(g_file_set_contents(path, "[Updates]\nSoftware=-4\nSignature=../../x\n", -1, nullptr));
    CHECK(xd_updates_state_read(path, &st) && st.software == 0 && st.signature[0] == '\0');
    g_unlink(path);
    g_rmdir(dir);
    g_free(path);
    g_free(dir);

    /* Importance. */
    CHECK(xd_update_rank_of_package(PK_INFO_ENUM_SECURITY, PK_INFO_ENUM_UNKNOWN) ==
          XD_RANK_SECURITY);
    CHECK(xd_update_rank_of_package(PK_INFO_ENUM_NORMAL, PK_INFO_ENUM_CRITICAL) ==
          XD_RANK_CRITICAL);
    CHECK(xd_update_rank_of_package(PK_INFO_ENUM_AVAILABLE, PK_INFO_ENUM_UNKNOWN) ==
          XD_RANK_UNKNOWN);
    CHECK(strcmp(xd_update_rank_name(XD_RANK_BUGFIX), "Bug fix") == 0);
    CHECK(strcmp(xd_update_rank_name(XD_RANK_UNKNOWN), "") == 0);
    CHECK(xd_update_rank_important(XD_RANK_SECURITY) && !xd_update_rank_important(XD_RANK_BUGFIX));
    CHECK(xd_update_rank_of_urgency(4) == XD_RANK_CRITICAL);
    CHECK(xd_update_rank_of_urgency(2) == XD_RANK_RECOMMENDED);
    CHECK(xd_update_rank_of_urgency(77) == XD_RANK_UNKNOWN);
    CHECK(xd_update_restart_system(PK_RESTART_ENUM_SECURITY_SYSTEM));
    CHECK(!xd_update_restart_system(PK_RESTART_ENUM_SESSION));
    CHECK(xd_update_restart_text(PK_RESTART_ENUM_NONE) == nullptr);
    CHECK(strstr(xd_update_restart_text(PK_RESTART_ENUM_SYSTEM), "restart") != nullptr);

    /* fwupd's release notes, checksums and locations. */
    char *text = xd_fwupd_markup_to_text("<p>This release fixes\n  security issues.</p>"
                                         "<ul><li>Fixes CVE-1 &amp; CVE-2</li><li>Faster "
                                         "<em>boot</em></li></ul><p>Thanks.</p>");
    CHECK(text != nullptr && strcmp(text, "This release fixes security issues.\n\n"
                                          "• Fixes CVE-1 & CVE-2\n• Faster boot\n\n"
                                          "Thanks.") == 0);
    g_free(text);
    text = xd_fwupd_markup_to_text("Plain text");
    CHECK(text != nullptr && strcmp(text, "Plain text") == 0);
    g_free(text);
    CHECK(xd_fwupd_markup_to_text(nullptr) == nullptr);
    GBytes *hello = g_bytes_new_static("hello", 5);
    const char *sha1 = "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d";
    const char *sha256 = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
    CHECK(xd_fwupd_checksum_ok(sha1, hello));
    char *both = g_strdup_printf("%s,%s", sha1, sha256);
    CHECK(xd_fwupd_checksum_ok(both, hello));
    g_free(both);
    /* SHA-256 given: a wrong one fails even with a right SHA-1. */
    both = g_strdup_printf("%s,%064d", sha1, 0);
    CHECK(!xd_fwupd_checksum_ok(both, hello));
    g_free(both);
    CHECK(!xd_fwupd_checksum_ok("5d41402abc4b2a76b9719d911017c592", hello)); /* MD5 only */
    CHECK(!xd_fwupd_checksum_ok(nullptr, hello) && !xd_fwupd_checksum_ok("", hello));
    g_bytes_unref(hello);
    const char *locs[] = {"http://x.invalid/a.cab", "https://cdn.fwupd.org/a.cab", nullptr};
    CHECK(strcmp(xd_fwupd_pick_location(locs, nullptr), "https://cdn.fwupd.org/a.cab") == 0);
    const char *plain[] = {"http://x.invalid/a.cab", nullptr};
    CHECK(xd_fwupd_pick_location(plain, nullptr) == nullptr);
    CHECK(strcmp(xd_fwupd_pick_location(nullptr, "file:///var/lib/a.cab"), "file:///var/lib/a.cab") == 0);

    /* The Host Security ID. */
    bool runtime = false;
    CHECK(xd_fwupd_hsi_level("HSI:2! (v1.9.6)", &runtime) == 2 && runtime);
    CHECK(xd_fwupd_hsi_level("HSI:0", &runtime) == 0 && !runtime);
    CHECK(xd_fwupd_hsi_level("HSI:INVALID:missing-data", &runtime) == -1);
    CHECK(xd_fwupd_hsi_level(nullptr, &runtime) == -1);
    CHECK(strcmp(xd_fwupd_hsi_result_text(1), "Enabled") == 0);
    CHECK(strcmp(xd_fwupd_hsi_result_text(6), "Not locked") == 0);
    CHECK(strcmp(xd_fwupd_hsi_result_text(99), "") == 0);
    CHECK(strcmp(xd_fwupd_attr_title("org.fwupd.hsi.Iommu", "Pre-boot DMA", nullptr),
                 "Pre-boot DMA") == 0);
    CHECK(strcmp(xd_fwupd_attr_title("org.fwupd.hsi.Uefi.SecureBoot", nullptr, "x"),
                 "UEFI Secure Boot") == 0);
    CHECK(strcmp(xd_fwupd_attr_title("org.example.New", "", "Something"), "Something") == 0);
    CHECK(strcmp(xd_fwupd_attr_title("org.example.New", nullptr, nullptr), "org.example.New") ==
          0);
}

/* Variable refresh rate and tearing (the compositor's present.c), and the
 * setting in monitors.ini. */
static void test_present(void) {
    CHECK(xd_vrr_mode_parse("fullscreen") == XD_VRR_FULLSCREEN);
    CHECK(xd_vrr_mode_parse("Always") == XD_VRR_ALWAYS);
    CHECK(xd_vrr_mode_parse("true") == XD_VRR_FULLSCREEN);
    CHECK(xd_vrr_mode_parse("off") == XD_VRR_OFF);
    CHECK(xd_vrr_mode_parse("false") == XD_VRR_OFF);
    CHECK(xd_vrr_mode_parse("sometimes") == XD_VRR_OFF);
    CHECK(xd_vrr_mode_parse(nullptr) == XD_VRR_OFF);
    for (int mode = XD_VRR_OFF; mode <= XD_VRR_ALWAYS; mode++)
        CHECK(xd_vrr_mode_parse(xd_vrr_mode_name((xd_vrr_mode)mode)) == mode);

    /* monitors.ini: kept with the rest; off when it isn't there. */
    char *dir = g_dir_make_tmp("xd-monitors-XXXXXX", nullptr);
    char *saved = g_strdup(g_getenv("XDG_CONFIG_HOME"));
    g_setenv("XDG_CONFIG_HOME", dir, TRUE);
    xd_monitor_config cfgs[2] = {
        {.key = "Acme Gamer 1", .enabled = true, .width = 2560, .height = 1440,
         .refresh_mhz = 143998, .scale = 1.0, .vrr = XD_VRR_FULLSCREEN},
        {.key = "Acme Office 2", .enabled = true, .width = 1920, .height = 1080, .scale = 1.0},
    };
    CHECK(xd_monitors_store(cfgs, 2));
    xd_monitor_config got;
    CHECK(xd_monitors_lookup("Acme Gamer 1", &got) && got.vrr == XD_VRR_FULLSCREEN &&
          got.refresh_mhz == 143998);
    CHECK(xd_monitors_lookup("Acme Office 2", &got) && got.vrr == XD_VRR_OFF);
    char *ini = g_build_filename(dir, "xeondesktop", "monitors.ini", nullptr);
    CHECK(g_file_set_contents(ini, "[Old]\nWidth=800\nHeight=600\n\n[Hand]\nAdaptiveSync= always \n",
                              -1, nullptr));
    CHECK(xd_monitors_lookup("Old", &got) && got.vrr == XD_VRR_OFF);
    CHECK(xd_monitors_lookup("Hand", &got) && got.vrr == XD_VRR_ALWAYS);
    g_free(ini);
    env_restore("XDG_CONFIG_HOME", saved);
    rm_tree(dir);
    g_free(dir);

    /* Off: never on, and a game may tear when it's alone on the monitor. */
    xd_present_state game = {.vrr = XD_VRR_OFF, .fullscreen = true, .scanout = true,
                             .async_hint = true};
    xd_present_choice present_choice = xd_present_choose(game);
    CHECK(!present_choice.set_vrr && present_choice.tearing);
    /* Only when it asked, and only when nothing is drawn over it. */
    xd_present_state present_state = game;
    present_state.async_hint = false;
    CHECK(!xd_present_choose(present_state).tearing);
    present_state = game;
    present_state.scanout = false;
    CHECK(!xd_present_choose(present_state).tearing);
    present_state = game;
    present_state.locked = true;
    CHECK(!xd_present_choose(present_state).tearing);
    /* On (full-screen windows): turned on when a game is in front, not
     * tearing in that same commit; on the desktop, turned off again. */
    present_state = game;
    present_state.vrr = XD_VRR_FULLSCREEN;
    present_choice = xd_present_choose(present_state);
    CHECK(present_choice.set_vrr && present_choice.vrr && !present_choice.tearing);
    present_state.vrr_on = true;
    present_choice = xd_present_choose(present_state);
    CHECK(!present_choice.set_vrr && present_choice.tearing);
    present_state.fullscreen = false;
    present_choice = xd_present_choose(present_state);
    CHECK(present_choice.set_vrr && !present_choice.vrr && !present_choice.tearing);
    present_state.vrr_on = false;
    CHECK(!xd_present_choose(present_state).set_vrr);
    /* A monitor that refused it isn't asked again. */
    present_state = game;
    present_state.vrr = XD_VRR_FULLSCREEN;
    present_state.vrr_rejected = true;
    present_choice = xd_present_choose(present_state);
    CHECK(!present_choice.set_vrr && present_choice.tearing);
    /* Always: on the desktop too, and the lock screen. */
    xd_present_state desk = {.vrr = XD_VRR_ALWAYS, .locked = true};
    present_choice = xd_present_choose(desk);
    CHECK(present_choice.set_vrr && present_choice.vrr && !present_choice.tearing);
    /* Turned off in the setting while on: off. */
    xd_present_state off = {.vrr = XD_VRR_OFF, .vrr_on = true, .fullscreen = true};
    present_choice = xd_present_choose(off);
    CHECK(present_choice.set_vrr && !present_choice.vrr);
}

/* A theme package's strip of `count` square frames of `size`, each a
 * `border`-pixel edge of `edge` around the frame's own fill. */
static void write_strip(const char *path, int size, int count, int border, uint32_t edge,
                        const uint32_t *fills) {
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size * count);
    cairo_t *cairo = cairo_create(surface);
    for (int i = 0; i < count; i++) {
        xd_fill_rect(cairo, (xd_rect){0, i * size, size, size}, xd_rgba_hex(edge));
        xd_fill_rect(cairo,
                     (xd_rect){border, i * size + border, size - 2 * border, size - 2 * border},
                     xd_rgba_hex(fills[i]));
    }
    cairo_destroy(cairo);
    CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(surface);
}

static uint32_t png_pixel(const char *path, int x, int y, int *width, int *height) {
    cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
    uint32_t px = 0;
    *width = *height = 0;
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        *width = cairo_image_surface_get_width(surface);
        *height = cairo_image_surface_get_height(surface);
        cairo_surface_flush(surface);
        const uint8_t *data = cairo_image_surface_get_data(surface);
        if (x < *width && y < *height)
            px = *(const uint32_t *)(const void *)(data +
                                                   y * cairo_image_surface_get_stride(surface) +
                                                   x * 4) &
                 0xFFFFFFu;
    }
    cairo_surface_destroy(surface);
    return px;
}

/* GTK drawn with a theme package: a tiny one made here (a push button with
 * 3-pixel sizing margins, a TrueSize check box, a button face colour),
 * its parts as border images and icons in the CSS, their images written
 * at 1x and 2x, what it leaves out (scroll bars) the base style's, and
 * its colours in the Qt palette. */
static void test_gtk_package(void) {
    char *dir = g_dir_make_tmp("xd-gtkpkg-XXXXXX", nullptr);
    char *pkgdir = g_build_filename(dir, "pkg", nullptr);
    char *images = g_build_filename(pkgdir, "images", nullptr);
    char *styles = g_build_filename(pkgdir, "styles", nullptr);
    CHECK(g_mkdir_with_parents(images, 0700) == 0 && g_mkdir_with_parents(styles, 0700) == 0);
    char *xml_path = g_build_filename(pkgdir, "theme.xml", nullptr);
    CHECK(g_file_set_contents(
        xml_path,
        "<xdtheme version=\"1\" name=\"Tiny\" base=\"luna\">\n"
        "  <variant id=\"v\" name=\"V\" size=\"Normal\" style=\"styles/v.xml\"/>\n"
        "</xdtheme>\n",
        -1, nullptr));
    g_free(xml_path);
    xml_path = g_build_filename(styles, "v.xml", nullptr);
    CHECK(g_file_set_contents(
        xml_path,
        "<style>\n  <sysmetrics>\n    <color name=\"BtnFace\" value=\"200 100 50\"/>\n"
        "  </sysmetrics>\n  <class name=\"Button\">\n"
        "    <part name=\"PushButton\" BgType=\"ImageFile\" ImageFile=\"images/button.png\"\n"
        "          ImageCount=\"5\" ImageLayout=\"Vertical\" SizingMargins=\"3 3 3 3\"\n"
        "          ContentMargins=\"2 2 2 2\" SizingType=\"Stretch\" TextColor=\"0 0 0\"/>\n"
        "    <part name=\"PushButton\" state=\"Disabled\" TextColor=\"128 128 128\"/>\n"
        "    <part name=\"CheckBox\" BgType=\"ImageFile\" ImageFile=\"images/check.png\"\n"
        "          ImageCount=\"12\" ImageLayout=\"Vertical\" SizingType=\"TrueSize\"/>\n"
        "  </class>\n</style>\n",
        -1, nullptr));
    g_free(xml_path);
    static const uint32_t button_fills[5] = {0x40C040, 0x4040C0, 0xC04040, 0x808080, 0xC0C040};
    uint32_t check_fills[12];
    for (int i = 0; i < 12; i++)
        check_fills[i] = 0x101010u * (uint32_t)(i + 1);
    xml_path = g_build_filename(images, "button.png", nullptr);
    write_strip(xml_path, 10, 5, 3, 0x203060, button_fills);
    g_free(xml_path);
    xml_path = g_build_filename(images, "check.png", nullptr);
    write_strip(xml_path, 13, 12, 1, 0x000000, check_fills);
    g_free(xml_path);

    GError *error = nullptr;
    xd_pkg *pkg = xd_pkg_load(pkgdir, nullptr, XD_SOURCE_ROOT "/data/theme/uxtheme.ini", &error);
    CHECK(pkg != nullptr);
    if (pkg == nullptr) {
        fprintf(stderr, "%s\n", error->message);
        g_error_free(error);
        rm_tree(dir);
        g_free(styles);
        g_free(images);
        g_free(pkgdir);
        g_free(dir);
        return;
    }
    xd_theme_use_package(pkg, XD_FILTER_NEAREST, true);
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &scheme, 1.0);
    CHECK(theme.pkg == pkg && theme.style == XD_STYLE_LUNA);

    xd_strbuf css = {0};
    xd_theme_gtk_css(&theme, &css);
    const char *text = xd_strbuf_cstr(&css);
    /* The button: its image cut at its margins, the middle inside the
     * content margins (exact across), each state its own frame. */
    CHECK(strstr(text, "border-image: -gtk-scaled(url(\"assets/pkg/button-pushbutton-1-b.png\"), "
                       "url(\"assets/pkg/button-pushbutton-1-b@2.png\")) 3 3 3 3 stretch;") !=
          nullptr);
    CHECK(strstr(text, "assets/pkg/button-pushbutton-2-b.png") != nullptr); /* hot */
    CHECK(strstr(text, "assets/pkg/button-pushbutton-5-b.png") != nullptr); /* default */
    CHECK(strstr(text, "border-width: 2px 3px 2px 3px;") != nullptr);
    CHECK(strstr(text, "#808080") != nullptr); /* the disabled text colour */
    /* The check box as GTK's icon, at its size. */
    CHECK(strstr(text, "checkbutton check:checked {\n  -gtk-icon-source: "
                       "-gtk-scaled(url(\"assets/pkg/button-checkbox-5-w.png\")") != nullptr);
    CHECK(strstr(text, "min-width: 13px;") != nullptr);
    /* The package's button face, as GTK's background. */
    CHECK(strstr(text, "@define-color theme_bg_color #c86432;") != nullptr);
    /* Scroll bars it doesn't style: Luna's, as generated without it. */
    CHECK(strstr(text, "assets/pkg/scrollbar") == nullptr);
    CHECK(strstr(text, "scrollbar slider, scrollbar button {") != nullptr);

    /* The images it names, at 1x and 2x. */
    char *gtk3 = g_build_filename(dir, "gtk-3.0", nullptr);
    CHECK(g_mkdir_with_parents(gtk3, 0700) == 0);
    xd_strbuf err = {0};
    CHECK(xd_gtk_theme_package_images(XD_STYLE_CLASSIC, &scheme, gtk3, &err));
    GArray *list = xd_gtk_pkg_images(&theme);
    CHECK(list->len > 10);
    for (guint i = 0; i < list->len; i++) {
        const xd_gtk_pkg_image *img = &g_array_index(list, xd_gtk_pkg_image, i);
        for (int scale = 1; scale <= 2; scale++) {
            char *name = g_strdup_printf("%s%s.png", img->name, scale == 1 ? "" : "@2");
            char *path = g_build_filename(gtk3, XD_GTK_PKG_ASSETS, name, nullptr);
            int width, height;
            (void)png_pixel(path, 0, 0, &width, &height);
            CHECK(width == img->w * scale && height == img->h * scale);
            g_free(path);
            g_free(name);
        }
    }
    g_array_unref(list);
    int width, height;
    char *hot = g_build_filename(gtk3, XD_GTK_PKG_ASSETS, "button-pushbutton-2-b.png", nullptr);
    CHECK(png_pixel(hot, 0, 0, &width, &height) == 0x203060 && width == 10 && height == 10);
    CHECK(png_pixel(hot, 5, 5, &width, &height) == 0x4040C0);
    g_free(hot);
    hot = g_build_filename(gtk3, XD_GTK_PKG_ASSETS, "button-pushbutton-2-b@2.png", nullptr);
    CHECK(png_pixel(hot, 5, 0, &width, &height) == 0x203060 &&
          png_pixel(hot, 10, 10, &width, &height) == 0x4040C0);
    g_free(hot);
    char *checked = g_build_filename(gtk3, XD_GTK_PKG_ASSETS, "button-checkbox-5-w.png", nullptr);
    CHECK(png_pixel(checked, 6, 6, &width, &height) == check_fills[4] && width == 13 &&
          height == 13);
    g_free(checked);

    /* All of it parses, images and all, read from beside gtk.css. */
    char *css_path = g_build_filename(gtk3, "gtk.css", nullptr);
    CHECK(g_file_set_contents(css_path, css.data, (gssize)css.len, nullptr));
    GtkCssProvider *provider = gtk_css_provider_new();
    g_signal_connect(provider, "parsing-error", G_CALLBACK(on_css_error), nullptr);
    css_errors = 0;
    CHECK(gtk_css_provider_load_from_path(provider, css_path, nullptr));
    CHECK(css_errors == 0);
    g_object_unref(provider);
    g_free(css_path);

    /* Qt: the same colours. */
    xd_strbuf json = {0};
    xd_qt_palette_json(&theme, &json);
    JsonParser *parser = json_parser_new();
    CHECK(json_parser_load_from_data(parser, xd_strbuf_cstr(&json), (gssize)json.len, nullptr));
    JsonNode *root = json_parser_get_root(parser);
    char *color = root != nullptr ? qt_palette_color(root, "SystemPalette", "Window") : nullptr;
    CHECK(g_strcmp0(color, "#c86432") == 0);
    g_free(color);
    g_object_unref(parser);
    xd_strbuf_free(&json);

    /* Without a package: none of it, and the images go. */
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_theme_init(&theme, XD_STYLE_LUNA, &scheme, 1.0);
    xd_strbuf_clear(&css);
    xd_theme_gtk_css(&theme, &css);
    CHECK(strstr(xd_strbuf_cstr(&css), "assets/pkg/") == nullptr);
    CHECK(xd_gtk_theme_package_images(XD_STYLE_LUNA, &scheme, gtk3, &err));
    char *gone = g_build_filename(gtk3, XD_GTK_PKG_ASSETS, "button-pushbutton-1-b.png", nullptr);
    CHECK(!g_file_test(gone, G_FILE_TEST_EXISTS));
    g_free(gone);

    xd_pkg_free(pkg);
    xd_strbuf_free(&err);
    xd_strbuf_free(&css);
    rm_tree(dir);
    g_free(gtk3);
    g_free(styles);
    g_free(images);
    g_free(pkgdir);
    g_free(dir);
}

/* ---- Explorer's panes from a theme package (shell/NAME.xml) ---- */

static void pane_file(const char *root, const char *rel, const char *text) {
    char *path = g_build_filename(root, rel, nullptr);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    CHECK(g_file_set_contents(path, text, -1, nullptr));
    g_free(dir);
    g_free(path);
}

static void pane_png(const char *root, const char *rel, int width, int height, uint32_t rgb) {
    char *path = g_build_filename(root, rel, nullptr);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cairo = cairo_create(surface);
    xd_fill_rect(cairo, (xd_rect){0, 0, width, height}, xd_rgba_hex(rgb));
    cairo_destroy(cairo);
    CHECK(cairo_surface_write_to_png(surface, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(surface);
    g_free(dir);
    g_free(path);
}

static uint32_t rgba_to_hex(xd_rgba color) {
    return (uint32_t)lround(color.r * 255) << 16 | (uint32_t)lround(color.g * 255) << 8 |
           (uint32_t)lround(color.b * 255);
}

/* A w x (h * frames) strip of frames, each pixel `color(frame, x, y)`. */
static void pkg_png_frames(const char *root, const char *name, int width, int height, int frames,
                           uint32_t (*color)(int frame, int x, int y)) {
    uint32_t *px = g_new(uint32_t, (gsize)(width * height * frames));
    for (int frame_index = 0; frame_index < frames; frame_index++)
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                px[(frame_index * height + y) * width + x] = color(frame_index, x, y);
    pkg_png(root, name, width, height * frames, px);
    g_free(px);
}

/* DWMWindow's edge pieces, as 7's are laid out (uxtheme.ini): the outer
 * edge's columns, the client edge's light and dark lines, glass between;
 * the inactive frame all one grey but its client edge's light line. */
static uint32_t dwm_left(int frame, int x, [[maybe_unused]] int y) {
    if (frame == 1)
        return x == 8 ? 0x00AA01 : 0x444444;
    return x < 3 ? 0x110000 : x == 8 ? 0x00AA00 : x > 8 ? 0x0000AA : 0x777777;
}
static uint32_t dwm_right(int frame, int x, [[maybe_unused]] int y) {
    if (frame == 1)
        return 0x444444;
    return x < 2 ? 0x0000BB : x == 2 ? 0x00BB00 : x > 7 ? 0x220000 : 0x777777;
}
static uint32_t dwm_top(int frame, [[maybe_unused]] int x, int y) {
    if (frame == 1)
        return 0x444444;
    return y == 24 ? 0xCC0000 : y > 24 ? 0x660000 : 0x777777;
}
static uint32_t dwm_bottom(int frame, [[maybe_unused]] int x, int y) {
    if (frame == 1)
        return 0x444444;
    return y < 2 ? 0x000066 : y == 2 ? 0x0000CC : 0x333333;
}

/* Vista's and 7's deduced parts drawn: DWMWindow's edges and reflections
 * on Aero's glass frame, and Alt+Tab from AltTab; and names Wine's
 * vssym32.h documents (MonthCal, StartPanel's later parts) as numbers. */
static void test_theme_package_dwm(void) {
    const char *names = XD_SOURCE_ROOT "/data/theme/uxtheme.ini";
    char *root = g_dir_make_tmp("xd-pkg-XXXXXX", nullptr);
    char *dir = g_build_filename(root, "styles", nullptr);
    CHECK(g_mkdir(dir, 0755) == 0);
    g_free(dir);
    dir = g_build_filename(root, "images", nullptr);
    CHECK(g_mkdir(dir, 0755) == 0);
    g_free(dir);
    pkg_text(root, "theme.xml",
             "<xdtheme version=\"1\" name=\"DWM Test\" base=\"aero\">\n"
             "  <variant id=\"a\" name=\"A\" size=\"normal\" style=\"styles/a.xml\"/>\n"
             "</xdtheme>\n");
    pkg_text(root, "styles/a.xml",
             "<style>\n"
             "  <class name=\"DWMWindow\">\n"
             "    <part name=\"CloseButton\" ImageFile=\"images/close.png\"/>\n"
             "    <part name=\"FrameTop\" ImageFile=\"images/top.png\" ImageCount=\"2\"\n"
             "          ImageLayout=\"Vertical\"/>\n"
             "    <part name=\"FrameBottom\" ImageFile=\"images/bottom.png\" ImageCount=\"2\"\n"
             "          ImageLayout=\"Vertical\"/>\n"
             "    <part name=\"FrameLeft\" ImageFile=\"images/left.png\" ImageCount=\"2\"\n"
             "          ImageLayout=\"Vertical\"/>\n"
             "    <part name=\"FrameRight\" ImageFile=\"images/right.png\" ImageCount=\"2\"\n"
             "          ImageLayout=\"Vertical\"/>\n"
             "    <part name=\"ReflectionStreaks\" ImageFile=\"images/streaks.png\"/>\n"
             "    <part name=\"ReflectionStreaks\" state=\"1\" Opacity=\"100\"/>\n"
             "    <part name=\"ReflectionStreaks\" state=\"2\" Opacity=\"0\"/>\n"
             "  </class>\n"
             "  <class name=\"AltTab\">\n"
             "    <part ImageFile=\"images/box.png\"/>\n"
             "    <part name=\"SelectedItem\" ImageFile=\"images/selected.png\"/>\n"
             "    <part name=\"Title\" TextColor=\"1 2 3\"/>\n"
             "  </class>\n"
             "  <class name=\"MonthCal\"><part name=\"GridCell\" TextColor=\"0 102 204\"/></class>\n"
             "  <class name=\"StartPanel\">\n"
             "    <part name=\"LogoffSplitButtonDropdown\" TextColor=\"1 1 1\"/>\n"
             "  </class>\n"
             "</style>\n");
    pkg_png_solid(root, "close.png", 4, 4, 0xE00000);
    pkg_png_frames(root, "left.png", 11, 3, 2, dwm_left);
    pkg_png_frames(root, "right.png", 11, 3, 2, dwm_right);
    pkg_png_frames(root, "top.png", 21, 27, 2, dwm_top);
    pkg_png_frames(root, "bottom.png", 21, 11, 2, dwm_bottom);
    pkg_png_solid(root, "streaks.png", 50, 80, 0xABCDEF);
    pkg_png_solid(root, "box.png", 4, 4, 0x204060);
    pkg_png_solid(root, "selected.png", 4, 4, 0x60C020);
    GError *error = nullptr;
    xd_pkg *package = xd_pkg_load(root, nullptr, names, &error);
    CHECK(package != nullptr && error == nullptr);
    if (package == nullptr) {
        g_clear_error(&error);
        rm_tree(root);
        g_free(root);
        return;
    }
    /* Documented in vssym32.h: stored by number, found by name or number. */
    CHECK(prop_is(package, "MonthCal", "6", nullptr, "TextColor", "0 102 204"));
    CHECK(prop_is(package, "StartPanel", "19", nullptr, "TextColor", "1 1 1"));

    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_scheme sc;
    xd_scheme_init_default(&sc);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_AERO, &sc, 1.0);
    theme.glass = true;
    /* 200 x 100: Aero's 8 px border and 30 px top, so the client area is
     * 8, 30, 184 x 62. */
    for (int active = 1; active >= 0; active--) {
        cairo_t *cairo;
        cairo_surface_t *surface = pkg_canvas(200, 100, &cairo);
        xd_frame frame = {.title = "", .active = active, .buttons = XD_CAPBTN_CLOSE};
        xd_theme_draw_frame(&theme, cairo, (xd_rect){0, 0, 200, 100}, &frame);
        cairo_destroy(cairo);
        if (active) {
            /* The client edge: light then dark lines just outside it. */
            CHECK(pixel(surface, 6, 60) == 0x00AA00 && pixel(surface, 7, 60) == 0x0000AA);
            CHECK(pixel(surface, 192, 60) == 0x0000BB && pixel(surface, 193, 60) == 0x00BB00);
            CHECK(pixel(surface, 100, 28) == 0xCC0000 && pixel(surface, 100, 29) == 0x660000);
            CHECK(pixel(surface, 100, 92) == 0x000066 && pixel(surface, 100, 93) == 0x0000CC);
            /* The outer edge, on the window's own edges. */
            CHECK(pixel(surface, 0, 60) == 0x110000 && pixel(surface, 199, 60) == 0x220000);
            CHECK(pixel(surface, 100, 3) == 0x777777 && pixel(surface, 100, 98) == 0x333333);
            /* The streaks over the glass, at Opacity 100. */
            CHECK(pixel(surface, 4, 50) == 0xABCDEF);
        } else {
            CHECK(pixel(surface, 6, 60) == 0x00AA01 && pixel(surface, 100, 3) == 0x444444);
            /* Opacity 0: no streaks. */
            CHECK(pixel(surface, 4, 50) != 0xABCDEF);
        }
        cairo_surface_destroy(surface);
    }

    /* Alt+Tab: the box, the selected window's frame (the second of
     * three), the title above the icons. */
    int width = 0, height = 0;
    xd_theme_switcher_size(&theme, 3, &width, &height);
    CHECK(width == 2 * 12 + 5 * 52 && height == 2 * 12 + 52 + 6 + 24);
    cairo_t *cairo;
    cairo_surface_t *surface = pkg_canvas(width, height, &cairo);
    cairo_surface_t *icons[3] = {nullptr, nullptr, nullptr};
    xd_theme_draw_switcher(&theme, cairo, (xd_rect){0, 0, width, height}, icons, 3, 1, "Notepad");
    cairo_destroy(cairo);
    int y = 12 + 24 + 6 + 2; /* inside the icons' row, above the icon */
    CHECK(pixel(surface, 1, 1) == 0x204060 && pixel(surface, width - 2, height - 2) == 0x204060);
    CHECK(pixel(surface, width / 2, y) == 0x60C020); /* the middle one is selected */
    CHECK(pixel(surface, width / 2 - 52, y) == 0x204060 &&
          pixel(surface, width / 2 + 52, y) == 0x204060);
    cairo_surface_destroy(surface);

    /* Without the package: Windows 2000's and XP's box, the selection a
     * square in the selection colour round the icon, the title below. */
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_theme_init(&theme, XD_STYLE_CLASSIC, &sc, 1.0);
    xd_theme_switcher_size(&theme, 3, &width, &height);
    CHECK(width == 2 * 10 + 5 * 43 && height == 2 * 10 + 43 + 8 + 24);
    surface = pkg_canvas(width, height, &cairo);
    xd_theme_draw_switcher(&theme, cairo, (xd_rect){0, 0, width, height}, icons, 3, 1, "Notepad");
    cairo_destroy(cairo);
    uint32_t face = (uint32_t)sc.colors[XD_COLOR_BUTTON_FACE].r << 16 |
                    (uint32_t)sc.colors[XD_COLOR_BUTTON_FACE].g << 8 |
                    sc.colors[XD_COLOR_BUTTON_FACE].b;
    uint32_t hilight = (uint32_t)sc.colors[XD_COLOR_HILIGHT].r << 16 |
                       (uint32_t)sc.colors[XD_COLOR_HILIGHT].g << 8 | sc.colors[XD_COLOR_HILIGHT].b;
    int sq = (43 - 40) / 2; /* the square: the 32 px icon and 4 px round it */
    CHECK(pixel(surface, 5, 5) == face);
    CHECK(pixel(surface, width / 2, 10 + sq) == hilight &&
          pixel(surface, width / 2 - 43, 10 + sq) == face);
    cairo_surface_destroy(surface);

    xd_pkg_free(package);
    rm_tree(root);
    g_free(root);
}

static void test_explorer_panes(void) {
    char *root = g_dir_make_tmp("xd-panes-XXXXXX", nullptr);
    char *names = g_build_filename(XD_SOURCE_ROOT, "data", "theme", "uxtheme.ini", nullptr);
    pane_file(root, "theme.xml",
              "<xdtheme version=\"1\" name=\"Test\" base=\"luna\">"
              "<variant id=\"blue\" name=\"Blue\" size=\"Normal\" style=\"styles/blue.xml\""
              " shell=\"shell/blue.xml\"/>"
              "<variant id=\"bare\" name=\"Bare\" size=\"Normal\" style=\"styles/blue.xml\"/>"
              "<variant id=\"lost\" name=\"Lost\" size=\"Normal\" style=\"styles/blue.xml\""
              " shell=\"shell/missing.xml\"/></xdtheme>");
    /* The style's own ExplorerBar (as luna.msstyles has, for IE's bars):
     * the shell's parts win where they say otherwise. */
    pane_file(root, "styles/blue.xml",
              "<style><class name=\"ExplorerBar\">"
              "<part name=\"NormalGroupHead\" TextColor=\"1 2 3\" Font=\"Arial, 9\"/>"
              "<part name=\"NormalGroupExpand\" ImageCount=\"3\" TextColor=\"7 8 9\"/>"
              "</class></style>");
    pane_file(root, "shell/blue.xml",
              "<shell><class name=\"ExplorerBar\">"
              "<part BgType=\"BorderFill\" FillType=\"VertGradient\" GradientColor1=\"10 20 30\""
              " GradientColor2=\"10 20 30\" BorderColor=\"255 255 255\" BorderThickness=\"1 0 0 1\""
              " ContentMargins=\"13 12 12 13\" GroupSpacing=\"15\"/>"
              "<part name=\"NormalGroupHead\" BgType=\"ImageFile\" ImageFile=\"images/head.png\""
              " SizingType=\"Stretch\" SizingMargins=\"2 2 2 0\" ContentMargins=\"12 3 2 0\""
              " Font=\"Tahoma, 8, Bold\" TextColor=\"33 93 198\"/>"
              "<part name=\"NormalGroupHead\" state=\"Hot\" TextColor=\"66 142 255\"/>"
              "<part name=\"NormalGroupExpand\" state=\"Normal\" BgType=\"ImageFile\""
              " ImageFile=\"images/arrow.png\" ImageCount=\"1\" SizingType=\"TrueSize\""
              " OffsetType=\"TopRight\" Offset=\"-3 2\"/>"
              "<part name=\"NormalGroupBackground\" BgType=\"BorderFill\" FillType=\"Solid\""
              " FillColor=\"214 223 247\" BorderColor=\"255 255 255\" BorderThickness=\"1 1 0 1\""
              " ContentMargins=\"13 13 10 11\" Font=\"Tahoma, 8\" TextColor=\"33 93 198\""
              " LinkSpacing=\"4\" IconSpacing=\"6\"/>"
              "<part name=\"NormalGroupBackground\" state=\"Hot\" Font=\"Tahoma, 8, Underline\"/>"
              "</class><class name=\"TaskPane\"><part name=\"Details\" LinkSpacing=\"5\"/>"
              "</class></shell>");
    pane_png(root, "images/head.png", 186, 21, 0x112233);
    pane_png(root, "images/arrow.png", 21, 23, 0x445566);

    GError *err = nullptr;
    xd_pkg *package = xd_pkg_load(root, "blue", names, &err);
    CHECK(package != nullptr);
    if (package == nullptr) {
        g_clear_error(&err);
        g_free(names);
        g_free(root);
        return;
    }
    xd_rgba color;
    CHECK(xd_pkg_color(package, "ExplorerBar", "NormalGroupHead", nullptr, "TextColor", &color) &&
          rgba_to_hex(color) == 0x215DC6);
    CHECK(xd_pkg_color(package, "ExplorerBar", "NormalGroupHead", "Hot", "TextColor", &color) &&
          rgba_to_hex(color) == 0x428EFF);
    /* What the shell doesn't say, the style's part still does. */
    CHECK(
        xd_pkg_color(package, "ExplorerBar", "NormalGroupExpand", "Normal", "TextColor", &color) &&
        rgba_to_hex(color) == 0x070809);
    xd_font_spec font;
    bool under = true;
    CHECK(xd_pkg_font(package, "ExplorerBar", "NormalGroupHead", nullptr, "Font", &font, &under));
    CHECK(fabs(font.size_pt - 8) < 1e-9 && font.bold && !font.italic && !under &&
          strstr(font.family, "Tahoma") != nullptr);
    CHECK(xd_pkg_font(package, "ExplorerBar", "NormalGroupBackground", "Hot", "Font", &font,
                      &under) &&
          under && !font.bold);

    xd_theme_use_package(package, XD_FILTER_SMOOTH, true);
    xd_scheme scheme;
    xd_scheme_init_default(&scheme);
    xd_theme theme;
    xd_theme_init(&theme, XD_STYLE_LUNA, &scheme, 1.0);
    xd_taskpane_style st;
    xd_theme_taskpane_style(&theme, &st);
    CHECK(st.padding.left == 13 && st.padding.top == 12 && st.padding.bottom == 13);
    CHECK(st.group_spacing == 15);
    /* The title bar: the arrow (23) and the margin above it (2). */
    CHECK(st.header_height == 25);
    CHECK(st.body[0].left == 13 && st.body[0].top == 10 && st.body[0].bottom == 11);
    CHECK(st.link_spacing[0] == 4 && st.icon_gap[0] == 6 && st.details_spacing == 5);
    CHECK(!st.link_underline[0][0] && st.link_underline[0][1]);
    CHECK(rgba_to_hex(st.header_color[0][1]) == 0x428EFF);

    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 220, 100);
    cairo_t *cairo = cairo_create(surface);
    xd_theme_draw_taskpane(&theme, cairo, (xd_rect){0, 0, 220, 100});
    xd_theme_draw_taskpane_header(&theme, cairo, &st, (xd_rect){12, 12, 186, st.header_height},
                                  false, false, XD_STATE_NORMAL, nullptr);
    xd_theme_draw_taskpane_body(&theme, cairo, (xd_rect){12, 37, 186, 40}, false);
    cairo_destroy(cairo);
    CHECK(near(pixel(surface, 100, 5), 0x0A141E, 2)); /* the pane */
    CHECK(near(pixel(surface, 0, 50), 0xFFFFFF, 0)); /* its left border */
    CHECK(near(pixel(surface, 219, 50), 0x0A141E, 2)); /* none on the right */
    CHECK(near(pixel(surface, 60, 24), 0x112233, 2)); /* the title bar's image */
    /* The arrow 3 px in from the title bar's right, 2 px down. */
    CHECK(near(pixel(surface, 12 + 186 - 3 - 10, 12 + 2 + 11), 0x445566, 2));
    CHECK(near(pixel(surface, 12 + 186 - 2, 12 + 2 + 11), 0x112233, 2));
    CHECK(near(pixel(surface, 100, 50), 0xD6DFF7, 0)); /* the body */
    CHECK(near(pixel(surface, 12, 50), 0xFFFFFF, 0)); /* its left border */
    CHECK(near(pixel(surface, 100, 37), 0xD6DFF7, 0)); /* none on top */
    CHECK(near(pixel(surface, 100, 76), 0xFFFFFF, 0)); /* its bottom border */
    cairo_surface_destroy(surface);

    /* Vista's and 7's parts: not in this package, so not drawn. */
    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 4, 4);
    cairo = cairo_create(surface);
    CHECK(!xd_theme_draw_explorer_part(&theme, cairo, XD_EXPLORER_COMMAND_BAR, 0,
                                       (xd_rect){0, 0, 4, 4}));
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    xd_theme_use_package(nullptr, XD_FILTER_SMOOTH, true);
    xd_pkg_free(package);

    /* No shell: nothing of the task pane's in the package. */
    package = xd_pkg_load(root, "bare", names, &err);
    CHECK(package != nullptr &&
          xd_pkg_prop(package, "ExplorerBar", nullptr, nullptr, "GroupSpacing") == nullptr);
    xd_pkg_free(package);
    /* A shell named but missing: the package doesn't load. */
    package = xd_pkg_load(root, "lost", names, &err);
    CHECK(package == nullptr && err != nullptr);
    g_clear_error(&err);

    const char *cmd[] = {"rm", "-rf", root, nullptr};
    g_spawn_sync(nullptr, (char **)(uintptr_t)cmd, nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                 nullptr, nullptr, nullptr, nullptr);
    g_free(names);
    g_free(root);
}

/* Previous Versions (versions.h) in scratch trees laid out as ZFS's and
 * Timeshift's, through XD_SNAPSHOT_ROOTS: never the real snapshots or
 * Snapper. */
static void set_versions_mtime(const char *root, const char *relative, gint64 seconds) {
    char *path = g_build_filename(root, relative, nullptr);
    struct utimbuf times = {.actime = (time_t)seconds, .modtime = (time_t)seconds};
    CHECK(g_utime(path, &times) == 0);
    g_free(path);
}

static gboolean is_versions_file_text(const char *root, const char *relative,
                                      const char *expected) {
    char *path = g_build_filename(root, relative, nullptr);
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        text = nullptr;
    gboolean is_same = g_strcmp0(text, expected) == 0;
    g_free(text);
    g_free(path);
    return is_same;
}

static void test_previous_versions(void) {
    char *scratch = g_dir_make_tmp("xd-versions-XXXXXX", nullptr);
    CHECK(scratch != nullptr);
    if (scratch == nullptr)
        return;
    char *root = realpath(scratch, nullptr);
    char *live = g_build_filename(root, "live", nullptr);
    char *zfs = g_build_filename(live, ".zfs", "snapshot", nullptr);
    char *backup = g_build_filename(root, "backup", nullptr);

    /* The item as it is now, and in three ZFS snapshots: s1 and s2 hold
     * the same notes.txt, s3 a newer one; only s1 has the folder. */
    put_file(live, "notes.txt", "now");
    put_file(live, "folder/a.txt", "a-now");
    put_file(live, "folder/extra.txt", "extra");
    put_file(zfs, "s1/notes.txt", "one");
    put_file(zfs, "s1/folder/a.txt", "a-one");
    put_file(zfs, "s1/folder/sub/b.txt", "b-one");
    put_file(zfs, "s2/notes.txt", "one");
    put_file(zfs, "s3/notes.txt", "two");
    put_file(zfs, "locked/notes.txt", "secret");
    set_versions_mtime(zfs, "s1/notes.txt", 1000);
    set_versions_mtime(zfs, "s2/notes.txt", 1000);
    set_versions_mtime(zfs, "s3/notes.txt", 2000);
    set_versions_mtime(live, "notes.txt", 5000);
    set_versions_mtime(live, "folder", 6000);
    set_versions_mtime(zfs, "s1/folder", 1500);
    set_versions_mtime(zfs, "s1", 1000000);
    set_versions_mtime(zfs, "s2", 2000000);
    set_versions_mtime(zfs, "s3", 3000000);
    set_versions_mtime(zfs, "locked", 4000000);
    char *locked = g_build_filename(zfs, "locked", nullptr);
    CHECK(g_chmod(locked, 0) == 0);

    /* A Timeshift rsync snapshot of the whole system, named by its time. */
    char *in_timeshift = g_build_filename("timeshift/snapshots/2026-01-02_03-04-05/localhost",
                                          live + 1, "notes.txt", nullptr);
    put_file(backup, in_timeshift, "shifted");
    set_versions_mtime(backup, in_timeshift, 3000);
    char *roots = g_strdup_printf("zfs=%s:timeshift=%s", live, backup);
    g_setenv("XD_SNAPSHOT_ROOTS", roots, TRUE);

    char *notes = g_build_filename(live, "notes.txt", nullptr);
    gboolean has_snapshots = FALSE;
    GPtrArray *versions = xd_versions_list(notes, &has_snapshots);
    CHECK(has_snapshots);
    /* Newest first: Timeshift's (2026), s3, then s2 standing for s1 too;
     * the locked snapshot is left out (unless we're root and can read it). */
    gboolean is_root = geteuid() == 0;
    CHECK(versions->len == (is_root ? 4u : 3u));
    if (versions->len >= 3 && !is_root) {
        const xd_version *shifted = g_ptr_array_index(versions, 0);
        const xd_version *two = g_ptr_array_index(versions, 1);
        const xd_version *one = g_ptr_array_index(versions, 2);
        char *expected = g_build_filename(backup, in_timeshift, nullptr);
        CHECK(g_strcmp0(shifted->path, expected) == 0);
        g_free(expected);
        GDateTime *taken = g_date_time_new_local(2026, 1, 2, 3, 4, 5);
        CHECK(shifted->snapshot_time == g_date_time_to_unix(taken));
        g_date_time_unref(taken);
        CHECK(shifted->modified_time == 3000 && shifted->size == 7 && !shifted->is_directory);
        CHECK(g_strcmp0(shifted->location, "Timeshift snapshot 2026-01-02_03-04-05") == 0);
        expected = g_build_filename(zfs, "s3", "notes.txt", nullptr);
        CHECK(g_strcmp0(two->path, expected) == 0);
        CHECK(two->snapshot_time == 3000000 && two->modified_time == 2000);
        CHECK(g_strcmp0(two->location, "ZFS snapshot s3") == 0);
        g_free(expected);
        expected = g_build_filename(zfs, "s2", "notes.txt", nullptr);
        CHECK(g_strcmp0(one->path, expected) == 0);
        g_free(expected);
    }
    g_ptr_array_unref(versions);

    /* A version the same as the item now says nothing new. Root reads the
     * locked snapshot, the newest ZFS one, so s3 comes after it. */
    put_file(live, "notes.txt", "shifted");
    set_versions_mtime(live, "notes.txt", 3000);
    versions = xd_versions_list(notes, nullptr);
    CHECK(versions->len == (is_root ? 3u : 2u));
    guint s3_index = is_root ? 1 : 0;
    if (versions->len > s3_index)
        CHECK(strstr(((const xd_version *)g_ptr_array_index(versions, s3_index))->path,
                     "/s3/") != nullptr);
    g_ptr_array_unref(versions);

    /* A folder, through a link to the folder it's in: the path as it
     * really is. */
    char *link = g_build_filename(root, "link", nullptr);
    CHECK(symlink(live, link) == 0);
    char *folder_by_link = g_build_filename(link, "folder", nullptr);
    versions = xd_versions_list(folder_by_link, &has_snapshots);
    CHECK(has_snapshots && versions->len == 1);
    char *folder_copy = g_build_filename(zfs, "s1", "folder", nullptr);
    if (versions->len == 1) {
        const xd_version *version = g_ptr_array_index(versions, 0);
        CHECK(version->is_directory && g_strcmp0(version->path, folder_copy) == 0);
    }
    g_ptr_array_unref(versions);
    g_free(folder_by_link);

    /* None of the item: an empty list; without any snapshots of its file
     * system, no tab at all. */
    put_file(root, "elsewhere/file.txt", "x");
    char *elsewhere = g_build_filename(root, "elsewhere", "file.txt", nullptr);
    versions = xd_versions_list(elsewhere, &has_snapshots);
    CHECK(has_snapshots && versions->len == 0); /* Timeshift has the whole system */
    g_ptr_array_unref(versions);
    char *only_zfs = g_strdup_printf("zfs=%s", live);
    g_setenv("XD_SNAPSHOT_ROOTS", only_zfs, TRUE);
    versions = xd_versions_list(elsewhere, &has_snapshots);
    CHECK(!has_snapshots && versions->len == 0);
    g_ptr_array_unref(versions);
    CHECK(!xd_versions_can_have_versions(elsewhere));
    CHECK(xd_versions_can_have_versions(notes));
    g_setenv("XD_SNAPSHOT_ROOTS", "", TRUE);
    CHECK(!xd_versions_can_have_versions(notes));
    g_setenv("XD_SNAPSHOT_ROOTS", roots, TRUE);
    CHECK(xd_versions_can_have_versions(elsewhere));
    g_free(only_zfs);

    /* Restoring a file replaces it, its dates too. */
    char *two_copy = g_build_filename(zfs, "s3", "notes.txt", nullptr);
    GError *error = nullptr;
    CHECK(xd_versions_restore(two_copy, notes, nullptr, &error));
    g_clear_error(&error);
    CHECK(is_versions_file_text(live, "notes.txt", "two"));
    GStatBuf info;
    CHECK(g_stat(notes, &info) == 0 && info.st_mtime == 2000);

    /* Restoring a folder copies its contents back in, leaving what the
     * snapshot didn't have. */
    char *folder = g_build_filename(live, "folder", nullptr);
    CHECK(xd_versions_restore(folder_copy, folder, nullptr, &error));
    g_clear_error(&error);
    CHECK(is_versions_file_text(live, "folder/a.txt", "a-one"));
    CHECK(is_versions_file_text(live, "folder/sub/b.txt", "b-one"));
    CHECK(is_versions_file_text(live, "folder/extra.txt", "extra"));

    /* Copying a version to a new place: a file and a folder. */
    char *copies = g_build_filename(root, "copies", nullptr);
    CHECK(g_mkdir(copies, 0700) == 0);
    char *copied_folder = g_build_filename(copies, "folder", nullptr);
    char *copied_file = g_build_filename(copies, "notes.txt", nullptr);
    CHECK(xd_versions_restore(folder_copy, copied_folder, nullptr, &error));
    g_clear_error(&error);
    CHECK(xd_versions_restore(two_copy, copied_file, nullptr, &error));
    g_clear_error(&error);
    CHECK(is_versions_file_text(copies, "folder/sub/b.txt", "b-one"));
    CHECK(is_versions_file_text(copies, "folder/a.txt", "a-one"));
    CHECK(is_versions_file_text(copies, "notes.txt", "two"));

    /* A link in the folder where the snapshot has a folder isn't
     * followed: nothing is written where it points. */
    put_file(zfs, "s1/folder/sub/c.txt", "c-one");
    char *outside = g_build_filename(root, "outside", nullptr);
    CHECK(g_mkdir(outside, 0700) == 0);
    char *sub = g_build_filename(folder, "sub", nullptr);
    rm_tree(sub);
    CHECK(symlink(outside, sub) == 0);
    CHECK(!xd_versions_restore(folder_copy, folder, nullptr, &error));
    CHECK(error != nullptr && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY));
    g_clear_error(&error);
    CHECK(is_versions_file_text(outside, "b.txt", nullptr));
    CHECK(is_versions_file_text(outside, "c.txt", nullptr));
    CHECK(g_file_test(sub, G_FILE_TEST_IS_SYMLINK));
    CHECK(is_versions_file_text(live, "folder/a.txt", "a-one"));

    g_unsetenv("XD_SNAPSHOT_ROOTS");
    CHECK(g_chmod(locked, 0700) == 0);
    rm_tree(scratch);
    g_free(sub);
    g_free(outside);
    g_free(copied_file);
    g_free(copied_folder);
    g_free(copies);
    g_free(folder);
    g_free(two_copy);
    g_free(elsewhere);
    g_free(folder_copy);
    g_free(link);
    g_free(notes);
    g_free(roots);
    g_free(in_timeshift);
    g_free(locked);
    g_free(backup);
    g_free(zfs);
    g_free(live);
    free(root);
    g_free(scratch);
}

/* The Open and Save As dialogs (src/portal/chooserlib.c): the portal's
 * filters as Files of type, which names they keep, the extension Save As
 * adds, and what's typed in File name. */
static void test_file_chooser(void) {
    GVariant *filters = g_variant_ref_sink(g_variant_new_parsed(
        "[('Text Documents (*.txt)', [(uint32 0, '*.txt')]),"
        " ('Pictures', [(uint32 0, '*.[Jj][Pp][Gg]'), (uint32 1, 'image/png')]),"
        " ('All Images', [(uint32 1, 'image/*')]),"
        " ('All Files', [(uint32 0, '*')])]"));
    GPtrArray *list = xd_chooser_filters_parse(filters);
    CHECK(list->len == 4);
    const xd_chooser_filter *text = g_ptr_array_index(list, 0);
    const xd_chooser_filter *pictures = g_ptr_array_index(list, 1);
    const xd_chooser_filter *images = g_ptr_array_index(list, 2);
    const xd_chooser_filter *all = g_ptr_array_index(list, 3);
    CHECK(strcmp(text->name, "Text Documents (*.txt)") == 0);
    CHECK(pictures->globs->len == 1 && pictures->mimes->len == 1);

    /* Globs regardless of case, as Windows matches; MIME types by the
     * name's type, wildcards too. */
    CHECK(xd_chooser_filter_matches(text, "Notes.txt", nullptr));
    CHECK(xd_chooser_filter_matches(text, "NOTES.TXT", nullptr));
    CHECK(!xd_chooser_filter_matches(text, "notes.txt.bak", nullptr));
    CHECK(xd_chooser_filter_matches(pictures, "photo.JPG", nullptr));
    CHECK(xd_chooser_filter_matches(pictures, "shot.png", nullptr));
    CHECK(xd_chooser_filter_matches(pictures, "unnamed", "image/png"));
    CHECK(!xd_chooser_filter_matches(pictures, "song.ogg", nullptr));
    CHECK(xd_chooser_filter_matches(images, "drawing.gif", nullptr));
    CHECK(!xd_chooser_filter_matches(images, "letter.txt", nullptr));
    CHECK(xd_chooser_filter_matches(all, "anything", nullptr));
    CHECK(xd_chooser_filter_matches(nullptr, "anything", nullptr));

    /* current_filter: the same filter, else one of its name; else none. */
    GVariant *current = g_variant_ref_sink(
        g_variant_new_parsed("('All Images', [(uint32 1, 'image/*')])"));
    CHECK(xd_chooser_filters_find(list, current) == 2);
    g_variant_unref(current);
    current = g_variant_ref_sink(g_variant_new_parsed("('Pictures', @a(us) [])"));
    CHECK(xd_chooser_filters_find(list, current) == 1);
    g_variant_unref(current);
    current = g_variant_ref_sink(g_variant_new_parsed("('Other', @a(us) [])"));
    CHECK(xd_chooser_filters_find(list, current) == -1);
    g_variant_unref(current);

    /* The extension Save As adds: the first glob's, brackets in lower case. */
    char *extension = xd_chooser_filter_get_extension(text);
    CHECK(g_strcmp0(extension, ".txt") == 0);
    g_free(extension);
    extension = xd_chooser_filter_get_extension(pictures);
    CHECK(g_strcmp0(extension, ".jpg") == 0);
    g_free(extension);
    CHECK(xd_chooser_filter_get_extension(images) == nullptr);
    CHECK(xd_chooser_filter_get_extension(all) == nullptr);

    static const struct {
        const char *typed, *saved;
    } names[] = {
        {"Report", "Report.txt"},          {"Report.txt", "Report.txt"},
        {"Report.doc", "Report.doc"},      {"\"Report\"", "Report"},
        {"Report.", "Report"},             {"Letters/Reply", "Letters/Reply.txt"},
        {"my.folder/Reply", "my.folder/Reply.txt"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        char *saved = xd_chooser_add_extension(names[i].typed, text);
        CHECK(strcmp(saved, names[i].saved) == 0);
        g_free(saved);
    }
    char *saved = xd_chooser_add_extension("Report", all);
    CHECK(strcmp(saved, "Report") == 0);
    g_free(saved);
    g_ptr_array_unref(list);
    g_variant_unref(filters);

    /* Nothing understood: no filters. */
    list = xd_chooser_filters_parse(nullptr);
    CHECK(list->len == 0);
    g_ptr_array_unref(list);

    /* What File name's text stands for. */
    char *scratch = g_dir_make_tmp("xd-chooser-XXXXXX", nullptr);
    CHECK(scratch != nullptr);
    if (scratch == nullptr)
        return;
    char *folder = realpath(scratch, nullptr);
    char *letters = g_build_filename(folder, "Letters", nullptr);
    CHECK(g_mkdir(letters, 0700) == 0);

    xd_typed_name typed = xd_chooser_parse_name(folder, "   ", false);
    CHECK(typed.kind == XD_TYPED_EMPTY && typed.paths == nullptr);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "notes.txt", false);
    char *expected = g_build_filename(folder, "notes.txt", nullptr);
    CHECK(typed.kind == XD_TYPED_FILE && g_strcmp0(typed.paths[0], expected) == 0 &&
          typed.paths[1] == nullptr);
    g_free(expected);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "Letters", false);
    CHECK(typed.kind == XD_TYPED_FOLDER && g_strcmp0(typed.paths[0], letters) == 0);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "Letters/../Letters/", false);
    CHECK(typed.kind == XD_TYPED_FOLDER && g_strcmp0(typed.paths[0], letters) == 0);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "*.txt;*.log", false);
    CHECK(typed.kind == XD_TYPED_PATTERN && g_strcmp0(typed.pattern, "*.txt;*.log") == 0 &&
          g_strcmp0(typed.paths[0], folder) == 0);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "Letters/a?c", false);
    CHECK(typed.kind == XD_TYPED_PATTERN && g_strcmp0(typed.pattern, "a?c") == 0 &&
          g_strcmp0(typed.paths[0], letters) == 0);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "/etc/hostname", false);
    CHECK(typed.kind == XD_TYPED_FILE && g_strcmp0(typed.paths[0], "/etc/hostname") == 0);
    xd_typed_name_free(&typed);

    typed = xd_chooser_parse_name(folder, "~/x.txt", false);
    expected = g_build_filename(g_get_home_dir(), "x.txt", nullptr);
    CHECK(typed.kind == XD_TYPED_FILE && g_strcmp0(typed.paths[0], expected) == 0);
    g_free(expected);
    xd_typed_name_free(&typed);

    /* Several names in quotes, when the dialog takes several; one in
     * quotes is just that name. */
    typed = xd_chooser_parse_name(folder, "\"a b.txt\" \"c.txt\"", true);
    char *first = g_build_filename(folder, "a b.txt", nullptr);
    char *second = g_build_filename(folder, "c.txt", nullptr);
    CHECK(typed.kind == XD_TYPED_FILE && g_strv_length(typed.paths) == 2 &&
          g_strcmp0(typed.paths[0], first) == 0 && g_strcmp0(typed.paths[1], second) == 0);
    xd_typed_name_free(&typed);
    typed = xd_chooser_parse_name(folder, "\"a b.txt\"", true);
    CHECK(typed.kind == XD_TYPED_FILE && g_strv_length(typed.paths) == 1 &&
          g_strcmp0(typed.paths[0], first) == 0);
    xd_typed_name_free(&typed);
    g_free(first);
    g_free(second);

    CHECK(xd_chooser_is_pattern("*.txt") && xd_chooser_is_pattern("a?c"));
    CHECK(!xd_chooser_is_pattern("notes.txt") && !xd_chooser_is_pattern(nullptr));

    g_rmdir(letters);
    g_rmdir(folder);
    g_free(letters);
    free(folder);
    g_free(scratch);
}

int main(void) {
    /* ssh-keygen and ssh-add run us as their askpass (test_keys_ssh). */
    if (getenv(XD_KEYS_ASKPASS_ENV) != nullptr)
        return xd_keys_askpass_main();
    test_str();
    test_arena();
    test_scheme();
    test_scheme_custom();
    test_shipped_schemes();
    test_luna_variants();
    test_px();
    test_render();
    test_hit();
    test_apps();
    test_menus();
    test_access_keys();
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
    test_ups();
    test_power_buttons();
    test_access_extra_settings();
    test_power_schemes();
    test_sound_scheme();
    test_gtk_css();
    test_gtk_package();
    test_passwd();
    test_intl();
    test_intl_custom();
    test_users();
    test_autologin_note();
    test_folder_options();
    test_input_settings();
    test_kbd_layouts();
    test_input_method();
    test_cursor_themes();
    test_file_types_edit();
    test_theme_settings();
    test_theme_packages();
    test_theme_sets();
    test_qt_style();
    test_qt_palette();
    test_fonts();
    test_bluetooth_info();
    test_joystick();
    test_bluetooth_settings();
    test_nightlight_settings();
    test_nightlight_math();
    test_vcgt();
    test_color_management();
    test_netconn();
    test_admin_services();
    test_admin_events();
    test_admin_disks();
    test_appwiz();
    test_flatpak_perms();
    test_updates();
    test_superbar();
    test_calendar();
    test_notices();
    test_snipping();
    test_recording();
    test_minefield();
    test_vboxdnd();
    test_launcher_entry();
    test_printers();
    test_scanners();
    test_schedtasks();
    test_startup_programs();
    test_startup_services();
    test_gpu();
    test_search_match();
    test_thumbnail_cache();
    test_favorites();
    test_launcher_fields();
    test_taskmgr_netstat();
    test_disk_cleanup();
    test_disk_cleanup_caches();
    test_disk_cleanup_system();
    test_luks();
    test_timedate_ntp();
    test_timedate_zone();
    test_privacy();
    test_keys_texts();
    test_keys_ssh();
    test_keys_gpg();
    test_network_drives();
    test_usershare();
    test_present();
    test_grab_keys();
    test_extract();
    test_image_scale();
    test_theme_package();
    test_theme_package_shell();
    test_theme_package_dwm();
    test_explorer_panes();
    test_previous_versions();
    test_file_chooser();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
