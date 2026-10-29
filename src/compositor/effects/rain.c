/* SPDX-License-Identifier: MIT
 * Digital Rain (xd-compositor-fx): a window built out of falling glyphs
 * as it opens or comes back, and broken up into them as it closes or
 * minimizes, after the screen saver of that name (src/saver/rain.c).
 *
 * Columns of glyphs fall in the window's footprint, each drop a bright,
 * near-white head with a trail fading to dark. A ragged front, each
 * column a little ahead of or behind its neighbours (rain_model.h),
 * crosses the window: a closing one breaks up from the top down, its
 * glyphs falling away and thinning out above the front; an opening one
 * is the same played backwards on the live window, so it builds from the
 * bottom up, the rain thickening as the front nears and each glyph that
 * lands flashing bright and turning into the window. Minimizing plays in
 * place, as Burn's does.
 *
 * The glyphs are drawn once, through Pango (src/saver/rain_glyphs.c, the
 * screen saver's sets), into an atlas texture for each glyph size in
 * device pixels (each monitor's scale); one shader pass a frame draws
 * the window, sampling a glyph from it for each cell, with no text laid
 * out per frame.
 *
 * It has no events of its own: Animations' events set to Digital Rain
 * are its, for the windows they and its page match, with its page's
 * duration. After Burn and Explode in impl_list.h (which keep their own
 * claims on closing windows), before the Add-On and Animations, which
 * leave its windows to it; Magic Lamp keeps its own.
 */
#include <drm_fourcc.h>
#include <math.h>
#include <string.h>
#include <wlr/render/wlr_texture.h>

#include "../../core/alloc.h"
#include "../../saver/rain_glyphs.h"
#include "glfx.h"
#include "rain_model.h"

static const char window_vertex[] = "uniform mat4 projection;\n"
                                    "uniform vec2 texcoord_scale;\n"
                                    "attribute vec2 position;\n"
                                    "attribute vec2 texcoord;\n"
                                    "varying vec2 v_texcoord;\n"
                                    "varying vec2 v_window;\n"
                                    "varying vec2 v_scale;\n"
                                    "void main() {\n"
                                    "    gl_Position = projection * vec4(position, 0.0, 1.0);\n"
                                    "    v_texcoord = texcoord * texcoord_scale;\n"
                                    "    v_window = texcoord;\n"
                                    "    v_scale = texcoord_scale;\n"
                                    "}\n";

/* rain_model.c's front, reveal, density, drops and colours, per cell of
 * glyphs, each column's speed, trail, start and jitter from hashes of
 * its number and the window's seed. Textures are premultiplied; the
 * atlas is white glyphs, their coverage in alpha. In two parts, joined
 * when it's compiled (C's longest string is shorter). */
static const char window_fragment_functions[] =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform XD_SAMPLER texture0;\n"
    "uniform sampler2D atlas;\n"
    "uniform float alpha;\n"
    "uniform vec2 window_size;\n"
    "uniform vec2 cell_size;\n"
    "uniform vec2 atlas_grid;\n"
    "uniform vec2 atlas_inset;\n"
    "uniform float glyph_count;\n"
    "uniform float progress;\n"
    "uniform float rows;\n"
    "uniform float reach;\n"
    "uniform float band;\n"
    "uniform float falloff;\n"
    "uniform float sparse;\n"
    "uniform float envelope;\n"
    "uniform float time;\n"
    "uniform float speed;\n"
    "uniform float seed;\n"
    "uniform vec3 base;\n"
    "uniform float window_colors;\n"
    "uniform float glow;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec2 v_window;\n"
    "varying vec2 v_scale;\n"
    "float hash(vec2 point) {\n"
    "    return fract(sin(dot(point, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "float get_front(float column) {\n"
    "    float jitter = hash(vec2(column, seed + 53.0)) * 2.0 - 1.0;\n"
    "    return clamp(progress, 0.0, 1.0) * (rows + 2.0 * reach + band) - reach - band\n"
    "           + jitter * reach;\n"
    "}\n"
    /* How lit the glyph in a cell is (0: none), whether it's its drop's
     * head, and the drop's number; with the column's front. */
    "float get_rain(vec2 cell, float front, out float head, out float drop) {\n"
    "    float column = cell.x;\n"
    "    float column_speed = speed * (0.6 + 0.8 * hash(vec2(column, seed)));\n"
    "    float trail = max(3.0, rows * (0.2 + 0.4 * hash(vec2(column, seed + 17.0))));\n"
    "    float start = hash(vec2(column, seed + 31.0));\n"
    "    float period = max(trail * (1.3 + 1.2 * start), 1.0);\n"
    "    float fall = time * column_speed + start * period * 7.0;\n"
    "    float below = fall - cell.y;\n"
    "    drop = floor(below / period);\n"
    "    float behind = floor(below - drop * period);\n"
    "    head = behind < 0.5 ? 1.0 : 0.0;\n"
    "    if (behind >= trail)\n"
    "        return 0.0;\n"
    "    float above = front - (cell.y + 0.5);\n"
    "    float density = above > 0.0\n"
    "                        ? (sparse + (1.0 - sparse) * exp(-above / falloff)) * envelope\n"
    "                        : envelope;\n"
    "    if (hash(vec2(column + drop * 0.618, seed + 71.0)) >= density)\n"
    "        return 0.0;\n"
    "    return 1.0 - behind / trail;\n"
    "}\n"
    "vec3 get_glyph_color(float brightness, vec3 color) {\n"
    "    if (brightness >= 1.0)\n"
    "        return 0.8 + 0.2 * color;\n"
    "    float level = brightness * (1.6 - 0.6 * brightness);\n"
    "    float white = max(0.0, brightness - 0.9) * 2.5;\n"
    "    return (color + (1.0 - color) * white) * level;\n"
    "}\n"
    /* The window's colour at a cell's middle, premultiplied. */
    "vec4 get_cell_pixel(vec2 cell) {\n"
    "    vec2 middle = clamp((cell + 0.5) * cell_size / window_size, 0.0, 1.0);\n"
    "    return texture2D(texture0, middle * v_scale);\n"
    "}\n";

static const char window_fragment_main[] =
    "void main() {\n"
    "    vec4 window = texture2D(texture0, v_texcoord) * alpha;\n"
    "    vec2 point = v_window * window_size / cell_size;\n"
    "    vec2 cell = floor(point);\n"
    "    float front = get_front(cell.x);\n"
    "    float reveal = clamp((cell.y + 0.5 - front) / band, 0.0, 1.0);\n"
    "    if (reveal >= 1.0) {\n"
    "        gl_FragColor = window;\n"
    "        return;\n"
    "    }\n"
    "    vec4 pixel = get_cell_pixel(cell);\n"
    /* Glyphs only where the window is (not its shadow, nor round
     * corners' outsides). */
    "    float footprint = step(0.02, pixel.a);\n"
    "    vec3 color = window_colors > 0.5 ? pixel.rgb / max(pixel.a, 0.001) : base;\n"
    "    float head = 0.0;\n"
    "    float drop = 0.0;\n"
    "    float lit = get_rain(cell, front, head, drop);\n"
    /* Over the desktop a trail fades out rather than to black. */
    "    float amount = clamp(lit * 1.5, 0.0, 1.0);\n"
    "    vec3 glyph_color = get_glyph_color(lit, color);\n"
    /* Below the front the window shows, cell by cell, a glyph landed
     * on it flashing bright and fading into it. */
    "    float shown = reveal > 0.0 ? 1.0 : 0.0;\n"
    "    if (reveal > 0.0) {\n"
    "        amount = max(amount * envelope, 1.0 - reveal);\n"
    "        glyph_color = get_glyph_color(1.0, color);\n"
    "    } else {\n"
    "        amount *= envelope;\n"
    "    }\n"
    "    float flicker = floor(time * 1.5 + hash(cell) * 8.0);\n"
    "    float glyph = floor(hash(vec2(cell.x * 7.0 + cell.y, flicker + drop * 3.0 + seed))\n"
    "                        * glyph_count);\n"
    "    glyph = min(glyph, glyph_count - 1.0);\n"
    "    vec2 slot = vec2(mod(glyph, atlas_grid.x), floor(glyph / atlas_grid.x));\n"
    "    vec2 inside = clamp(fract(point), atlas_inset, 1.0 - atlas_inset);\n"
    "    float mask = texture2D(atlas, (slot + inside) / atlas_grid).a;\n"
    "    float coverage = mask * amount * footprint;\n"
    "    vec4 color_out = vec4(glyph_color * coverage, coverage);\n"
    "    color_out += window * shown * (1.0 - coverage);\n"
    /* A soft light round each lit head, its own and its neighbours'
     * (added, as light is). */
    "    if (glow > 0.5) {\n"
    "        float light = 0.0;\n"
    "        for (int across = -1; across <= 1; across++) {\n"
    "            for (int down = -1; down <= 1; down++) {\n"
    "                vec2 other = cell + vec2(float(across), float(down));\n"
    "                float other_head = 0.0;\n"
    "                float other_drop = 0.0;\n"
    "                float other_front = get_front(other.x);\n"
    "                if (other.y + 0.5 >= other_front)\n"
    "                    continue;\n"
    "                float other_lit = get_rain(other, other_front, other_head, other_drop);\n"
    "                if (other_lit <= 0.0 || other_head < 0.5)\n"
    "                    continue;\n"
    "                vec2 offset = (point - other - 0.5) * cell_size / cell_size.y;\n"
    "                light += exp(-dot(offset, offset) * 2.5);\n"
    "            }\n"
    "        }\n"
    "        float strength = min(light, 1.0) * 0.35 * envelope * footprint;\n"
    "        color_out.rgb += color * strength;\n"
    "        color_out.a = min(color_out.a + strength * 0.5, 1.0);\n"
    "    }\n"
    "    gl_FragColor = color_out;\n"
    "}\n";

/* Compiled once, the first time a pass needs them, for 2D and external
 * textures; xd_glfx_finish destroys them, as glfx's own. */
typedef struct rain_shader {
    bool tried;
    xd_glfx_shader shader;
    GLint atlas, window_size, cell_size, atlas_grid, atlas_inset, glyph_count;
    GLint progress, rows, reach, band, falloff, sparse, envelope;
    GLint time, speed, seed, base, window_colors, glow;
} rain_shader;

static rain_shader window_shaders[2];

/* The rain's colours: the screen saver's four, Custom, or the window's. */
typedef enum rain_color : uint8_t {
    RAIN_COLOR_GREEN,
    RAIN_COLOR_AMBER,
    RAIN_COLOR_BLUE,
    RAIN_COLOR_WHITE,
    RAIN_COLOR_CUSTOM,
    RAIN_COLOR_WINDOW,
} rain_color;

typedef struct rain_options {
    int duration_ms;
    int glyph_size; /* px high at scale 1 */
    xd_rain_characters characters;
    double base[3]; /* the colour, but for the window's */
    bool window_colors;
    bool glow;
    char windows[512]; /* which of Animations' windows: xd_effects_match_view */
} rain_options;

/* The glyphs drawn at one size (device px), for windows on monitors of
 * one scale. */
typedef struct rain_atlas {
    int font_px; /* 0: none */
    struct wlr_texture *texture;
    int cell_width, cell_height; /* px */
    int columns, rows;           /* its grid of glyphs */
    int glyph_count;
    uint64_t used; /* when last asked for: the oldest goes first */
} rain_atlas;

/* Enough for every scale of a few monitors at once. */
enum { RAIN_ATLAS_COUNT = 4 };

typedef struct rain_state {
    xd_server *server;
    rain_options options;
    xd_animation_event_settings events[XD_ANIMATION_EVENT_COUNT]; /* Animations' */
    rain_atlas atlases[RAIN_ATLAS_COUNT];
    uint64_t atlas_clock;
    struct wl_list windows; /* rain_window.link */
    uint64_t seed;
} rain_state;

typedef struct rain_window {
    struct wl_list link; /* rain_state.windows */
    rain_state *state;
    xd_window_effect *effect;
    int duration_ms;
    bool appearing;  /* opening or restoring: built, live */
    double progress; /* 0: the whole window, 1: gone */
    double seconds;  /* since it began, always forwards: the drops fall */
    float seed;      /* its hashes' */
    double scale;    /* its monitor's: device px per layout unit */
    int font_px;     /* its atlas's, at that scale */
    xd_glfx_mesh mesh;
} rain_window;

static const xd_window_effect_impl window_impl;

/* ---- the glyphs ------------------------------------------------------------------- */

static void rain_free_atlas(rain_atlas *atlas) {
    if (atlas->texture != nullptr)
        wlr_texture_destroy(atlas->texture);
    *atlas = (rain_atlas){0};
}

static void rain_free_atlases(rain_state *state) {
    for (size_t i = 0; i < RAIN_ATLAS_COUNT; i++)
        rain_free_atlas(&state->atlases[i]);
}

static rain_atlas *rain_find_atlas(rain_state *state, int font_px) {
    for (size_t i = 0; i < RAIN_ATLAS_COUNT; i++) {
        if (state->atlases[i].font_px == font_px && state->atlases[i].texture != nullptr)
            return &state->atlases[i];
    }
    return nullptr;
}

/* The glyphs at `font_px`, white with their coverage in alpha, in a
 * grid about as wide as it's high. */
static bool rain_draw_atlas(rain_state *state, rain_atlas *atlas, int font_px) {
    xd_rain_characters wanted = state->options.characters;
    xd_rain_characters characters =
        xd_rain_pick_characters(wanted, wanted != XD_RAIN_KATAKANA || xd_rain_has_katakana());
    xd_rain_glyph glyphs[XD_RAIN_GLYPHS_MAX];
    size_t count = xd_rain_list_glyphs(characters, glyphs);
    if (count == 0)
        return false;
    int cell_width = 0, cell_height = 0;
    xd_rain_get_cell_size(font_px, &cell_width, &cell_height);
    double side = ceil(sqrt((double)count));
    int columns = (int)side;
    int rows = ((int)count + columns - 1) / columns;
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, columns * cell_width, rows * cell_height);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return false;
    }
    cairo_t *cairo = cairo_create(surface);
    cairo_set_source_rgb(cairo, 1, 1, 1);
    PangoLayout *layout = xd_rain_create_layout(cairo, characters, font_px);
    for (size_t i = 0; i < count; i++) {
        int column = (int)i % columns, row = (int)i / columns;
        xd_rain_draw_glyph(cairo, layout, &glyphs[i], column * cell_width, row * cell_height,
                           cell_width, cell_height);
    }
    g_object_unref(layout);
    cairo_destroy(cairo);
    cairo_surface_flush(surface);
    /* Cairo's ARGB32 is DRM's ARGB8888 in memory, premultiplied too. */
    struct wlr_texture *texture = wlr_texture_from_pixels(
        state->server->renderer, DRM_FORMAT_ARGB8888,
        (uint32_t)cairo_image_surface_get_stride(surface), (uint32_t)(columns * cell_width),
        (uint32_t)(rows * cell_height), cairo_image_surface_get_data(surface));
    cairo_surface_destroy(surface);
    if (texture == nullptr)
        return false;
    *atlas = (rain_atlas){
        .font_px = font_px,
        .texture = texture,
        .cell_width = cell_width,
        .cell_height = cell_height,
        .columns = columns,
        .rows = rows,
        .glyph_count = (int)count,
    };
    return true;
}

/* The atlas at `font_px`, drawn if there's none (outside a render pass:
 * from update), in the place of the one used longest ago. */
static rain_atlas *rain_ensure_atlas(rain_state *state, int font_px) {
    rain_atlas *atlas = rain_find_atlas(state, font_px);
    if (atlas == nullptr) {
        atlas = &state->atlases[0];
        for (size_t i = 1; i < RAIN_ATLAS_COUNT; i++) {
            if (state->atlases[i].used < atlas->used)
                atlas = &state->atlases[i];
        }
        rain_free_atlas(atlas);
        if (!rain_draw_atlas(state, atlas, font_px))
            return nullptr;
    }
    atlas->used = ++state->atlas_clock;
    return atlas;
}

/* ---- drawing -------------------------------------------------------------------- */

static const rain_shader *rain_get_shader(GLenum target) {
    rain_shader *entry = &window_shaders[target == GL_TEXTURE_EXTERNAL_OES ? 1 : 0];
    if (!entry->tried) {
        entry->tried = true;
        g_autofree char *fragment =
            g_strconcat(window_fragment_functions, window_fragment_main, nullptr);
        if (xd_glfx_init_shader(&entry->shader, window_vertex, fragment, target)) {
            GLuint program = entry->shader.program;
            entry->atlas = glGetUniformLocation(program, "atlas");
            entry->window_size = glGetUniformLocation(program, "window_size");
            entry->cell_size = glGetUniformLocation(program, "cell_size");
            entry->atlas_grid = glGetUniformLocation(program, "atlas_grid");
            entry->atlas_inset = glGetUniformLocation(program, "atlas_inset");
            entry->glyph_count = glGetUniformLocation(program, "glyph_count");
            entry->progress = glGetUniformLocation(program, "progress");
            entry->rows = glGetUniformLocation(program, "rows");
            entry->reach = glGetUniformLocation(program, "reach");
            entry->band = glGetUniformLocation(program, "band");
            entry->falloff = glGetUniformLocation(program, "falloff");
            entry->sparse = glGetUniformLocation(program, "sparse");
            entry->envelope = glGetUniformLocation(program, "envelope");
            entry->time = glGetUniformLocation(program, "time");
            entry->speed = glGetUniformLocation(program, "speed");
            entry->seed = glGetUniformLocation(program, "seed");
            entry->base = glGetUniformLocation(program, "base");
            entry->window_colors = glGetUniformLocation(program, "window_colors");
            entry->glow = glGetUniformLocation(program, "glow");
        }
    }
    return entry->shader.program != 0 ? entry : nullptr;
}

/* The window's rows of glyphs, and its cells' size in layout units. */
static int rain_get_rows(const rain_window *window, const rain_atlas *atlas, double scale,
                         double *cell_width, double *cell_height) {
    *cell_width = atlas->cell_width / scale;
    *cell_height = atlas->cell_height / scale;
    double rows = ceil(window->effect->texture.box.height / *cell_height);
    return rows < 1 ? 1 : (int)rows;
}

static void rain_draw_window(rain_window *window, const xd_glfx_draw *draw) {
    xd_window_effect *effect = window->effect;
    const rain_options *options = &window->state->options;
    const struct wlr_fbox *box = &effect->texture.box;
    xd_glfx_set_mesh_rect(&window->mesh, *box);
    const rain_shader *entry = rain_get_shader(effect->texture.target);
    rain_atlas *atlas = rain_find_atlas(window->state, window->font_px);
    if (entry == nullptr || atlas == nullptr) {
        /* No glyphs to draw it with: it fades instead. */
        xd_glfx_draw_mesh(draw, &window->mesh, &effect->texture, (float)(1 - window->progress));
        return;
    }
    double cell_width = 0, cell_height = 0;
    int rows = rain_get_rows(window, atlas, window->scale, &cell_width, &cell_height);
    struct fx_texture_attribs attribs = {0};
    fx_texture_get_attribs(atlas->texture, &attribs);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(attribs.target, attribs.tex);
    glTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(attribs.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(entry->shader.program);
    glUniform1i(entry->atlas, 1);
    glUniform2f(entry->window_size, (float)box->width, (float)box->height);
    glUniform2f(entry->cell_size, (float)cell_width, (float)cell_height);
    glUniform2f(entry->atlas_grid, (float)atlas->columns, (float)atlas->rows);
    glUniform2f(entry->atlas_inset, 0.5f / (float)atlas->cell_width,
                0.5f / (float)atlas->cell_height);
    glUniform1f(entry->glyph_count, (float)atlas->glyph_count);
    glUniform1f(entry->progress, (float)window->progress);
    glUniform1f(entry->rows, (float)rows);
    glUniform1f(entry->reach, (float)xd_digital_rain_get_reach(rows));
    glUniform1f(entry->band, (float)xd_digital_rain_get_band());
    glUniform1f(entry->falloff, (float)xd_digital_rain_get_falloff(rows));
    glUniform1f(entry->sparse, (float)xd_digital_rain_get_sparse());
    glUniform1f(entry->envelope, (float)xd_digital_rain_get_envelope(window->progress));
    glUniform1f(entry->time, (float)window->seconds);
    glUniform1f(entry->speed, (float)xd_digital_rain_get_fall_speed(rows));
    glUniform1f(entry->seed, window->seed);
    glUniform3f(entry->base, (float)options->base[0], (float)options->base[1],
                (float)options->base[2]);
    glUniform1f(entry->window_colors, options->window_colors ? 1.0f : 0.0f);
    glUniform1f(entry->glow, options->glow ? 1.0f : 0.0f);
    xd_glfx_draw_mesh_with(draw, &window->mesh, &effect->texture, &entry->shader, 1.0f);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(attribs.target, 0);
    glActiveTexture(GL_TEXTURE0);
}

/* ---- the window effect ----------------------------------------------------------- */

static void rain_begin(xd_window_effect *effect) {
    rain_window *window = effect->data;
    xd_glfx_init_mesh(&window->mesh, 1, 1);
}

static bool rain_update(xd_window_effect *effect, double seconds) {
    rain_window *window = effect->data;
    rain_state *state = window->state;
    double step = seconds * 1000.0 / XD_MAX(window->duration_ms, 1);
    window->progress = xd_animation_step_progress(window->progress, step, window->appearing);
    window->seconds += seconds;
    /* Its glyphs at its monitor's scale, drawn now if they're new:
     * outside the render pass draw runs in. */
    window->scale = XD_MAX(effect->held ? effect->held_scale : xd_view_scale(effect->view), 0.5);
    window->font_px = XD_MAX(4, (int)lround(state->options.glyph_size * window->scale));
    rain_ensure_atlas(state, window->font_px);
    return xd_animation_has_further(window->progress, window->appearing);
}

static struct wlr_fbox rain_get_bounds(xd_window_effect *effect) {
    return effect->texture.box;
}

static void rain_draw(xd_window_effect *effect, const xd_glfx_draw *draw) {
    rain_window *window = effect->data;
    rain_draw_window(window, draw);
}

static void rain_finish(xd_window_effect *effect) {
    rain_window *window = effect->data;
    wl_list_remove(&window->link);
    xd_glfx_free_mesh(&window->mesh);
    xd_free(window);
}

static const xd_window_effect_impl window_impl = {
    .name = "Digital Rain",
    .begin = rain_begin,
    .update = rain_update,
    .get_bounds = rain_get_bounds,
    .draw = rain_draw,
    .done = rain_finish,
};

/* ---- the effect ----------------------------------------------------------------- */

static void *rain_enable(xd_server *server) {
    rain_state *state = XD_NEW(rain_state);
    state->server = server;
    state->seed = 0x9E3779B97F4A7C15ULL;
    wl_list_init(&state->windows);
    return state;
}

static void rain_disable([[maybe_unused]] xd_server *server, void *data) {
    rain_state *state = data;
    rain_window *window, *next;
    wl_list_for_each_safe(window, next, &state->windows, link) {
        xd_glfx_end_window(window->effect); /* rain_finish frees it */
    }
    rain_free_atlases(state);
    xd_free(state);
}

static void rain_configure([[maybe_unused]] xd_server *server, void *data,
                           const xd_effects_settings *settings, size_t effect) {
    rain_state *state = data;
    rain_options *options = &state->options;
    options->duration_ms = xd_effects_get_int(settings, effect, "Duration");
    options->glyph_size = xd_effects_get_int(settings, effect, "GlyphSize");
    int characters = xd_effects_get_choice(settings, effect, "Characters");
    xd_rain_characters wanted = characters >= 0 && characters < XD_RAIN_CHARACTERS_COUNT
                                    ? (xd_rain_characters)characters
                                    : XD_RAIN_KATAKANA;
    /* Other glyphs: drawn again when next needed. */
    if (wanted != options->characters)
        rain_free_atlases(state);
    options->characters = wanted;
    int color = xd_effects_get_choice(settings, effect, "Color");
    uint32_t custom = xd_effects_get_color(settings, effect, "CustomColor");
    static const xd_rain_color saver_colors[] = {[RAIN_COLOR_GREEN] = XD_RAIN_GREEN,
                                                 [RAIN_COLOR_AMBER] = XD_RAIN_AMBER,
                                                 [RAIN_COLOR_BLUE] = XD_RAIN_BLUE,
                                                 [RAIN_COLOR_WHITE] = XD_RAIN_WHITE,
                                                 [RAIN_COLOR_CUSTOM] = XD_RAIN_CUSTOM};
    xd_rain_color saver_color = color >= 0 && (size_t)color < XD_ARRAY_LEN(saver_colors)
                                    ? saver_colors[color]
                                    : XD_RAIN_GREEN;
    xd_rgb rgb = {(uint8_t)(custom >> 24), (uint8_t)(custom >> 16), (uint8_t)(custom >> 8)};
    xd_rain_get_base_color(saver_color, rgb, options->base);
    options->window_colors = color == RAIN_COLOR_WINDOW;
    options->glow = xd_effects_get_bool(settings, effect, "Glow");
    /* Animations' events say which windows are ours: its settings come
     * with ours (every effect is told of every change). */
    xd_effects_get_animation_events(settings, state->events);
    snprintf(options->windows, sizeof(options->windows), "%s",
             xd_effects_get_text(settings, effect, "Windows"));
}

/* Whether it takes this window event: Animations' choice of it, for the
 * windows that event and its page match. Animations and the Add-On,
 * later in the list, then leave the window alone
 * (xd_effects_is_animated_before). */
static bool rain_is_wanted(const rain_state *state, const xd_view *view, xd_window_event_kind kind,
                           xd_animation_event *event) {
    if (view->fullscreen || !xd_effects_get_animation_event(kind, event))
        return false;
    const xd_animation_event_settings *chosen = &state->events[*event];
    bool event_match = chosen->effect == XD_ANIMATION_DIGITAL_RAIN &&
                       xd_effects_match_view(chosen->windows, view) &&
                       xd_effects_match_view(state->options.windows, view);
    return xd_animation_get_claim(XD_ANIMATION_DIGITAL_RAIN, *event, false, false, chosen->effect,
                                  event_match) != XD_ANIMATION_CLAIM_NONE;
}

static bool rain_animates([[maybe_unused]] xd_server *server, void *data, const xd_view *view,
                          xd_window_event_kind kind) {
    xd_animation_event event;
    return rain_is_wanted(data, view, kind, &event);
}

static void rain_start(rain_state *state, xd_view *view, xd_animation_event event) {
    rain_window *window = XD_NEW(rain_window);
    window->state = state;
    window->appearing = xd_animation_is_appearing(event);
    window->duration_ms = state->options.duration_ms;
    window->progress = xd_animation_get_start_progress(window->appearing);
    window->seed = (float)(++state->seed % 997) / 10.0f;
    /* Turned round midway (closed while it opens, minimized while it's
     * restored, or back): from where the one before had got to, the
     * same drops still falling. */
    xd_window_effect *before = xd_glfx_find_window(view, &window_impl);
    if (before != nullptr) {
        const rain_window *previous = before->data;
        window->progress = previous->progress;
        window->seconds = previous->seconds;
        window->seed = previous->seed;
    }
    window->effect = window->appearing ? xd_glfx_begin_window(view, &window_impl, window)
                                       : xd_glfx_begin_closing_window(view, &window_impl, window);
    if (window->effect == nullptr) {
        xd_free(window);
        return;
    }
    wl_list_insert(&state->windows, &window->link);
}

static void rain_handle_window_event([[maybe_unused]] xd_server *server, void *data,
                                     const xd_window_event *event) {
    rain_state *state = data;
    xd_view *view = event->view;
    xd_animation_event kind;
    /* Magic Lamp, Burn and Explode, before it, keep the windows they take. */
    if (!rain_is_wanted(state, view, event->kind, &kind) ||
        xd_effects_is_animated_before(&xd_effect_impl_digital_rain, view, event->kind))
        return;
    rain_start(state, view, kind);
}

const xd_effect_impl xd_effect_impl_digital_rain = {
    .id = "DigitalRain",
    .enable = rain_enable,
    .disable = rain_disable,
    .configure = rain_configure,
    .window_event = rain_handle_window_event,
    .animates = rain_animates,
};
