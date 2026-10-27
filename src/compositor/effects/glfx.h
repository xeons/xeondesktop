/* SPDX-License-Identifier: MIT
 * The OpenGL effect path (xd-compositor-fx only): windows drawn by an
 * effect, as meshes with shaders of its own, in their place among the
 * other windows.
 *
 * While an effect owns a window, each frame, before the scene graph
 * draws the output:
 *   1. the window's whole subtree (frame, client surfaces, subsurfaces,
 *      popups) is drawn into an offscreen buffer: its texture;
 *   2. the effect advances (update) and says where it will draw (bounds);
 *   3. it draws into a second buffer covering those bounds, with GLES
 *      calls of its own: meshes, shaders (draw);
 *   4. that buffer is shown by a scene buffer node at the top of the
 *      window's own tree, and the window's own buffers are drawn fully
 *      transparent (opacity 0; its blur and shadow are off).
 * So the effect's picture is stacked exactly where the window is:
 * windows above it stay above, raising it raises the picture, and input
 * still goes to the (invisible) window where it really is. The scene
 * graph damages what the node covered and covers. Direct scanout is off
 * while any window effect runs, and frames keep coming from the outputs'
 * frame events (not timers) until every effect is done.
 *
 * GL calls happen inside a render pass of SceneFX's renderer: its EGL
 * context is current and the target's framebuffer bound. Leave the GL
 * state as SceneFX expects it (draw_mesh does): no program, no texture,
 * no vertex arrays enabled, blending as it was.
 */
#ifndef XD_COMPOSITOR_EFFECTS_GLFX_H
#define XD_COMPOSITOR_EFFECTS_GLFX_H

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "effects.h"

/* ---- shaders -------------------------------------------------------------------- */

/* A program with the names draw_mesh sets: attributes `position` (vec2,
 * layout units) and `texcoord` (vec2: 0..1 across the window), uniforms
 * `projection` (mat4), `texcoord_scale` (vec2: what texcoord is multiplied
 * by to sample the texture, see xd_glfx_texture), `texture0` (the
 * texture) and `alpha` (float); any may be missing.
 * Sources are GLSL ES 1.00 without #version; a fragment shader declares
 * its texture `uniform XD_SAMPLER texture0;` and samples it with
 * texture2D, and is compiled for 2D textures or external ones (`target`
 * GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES) with XD_SAMPLER defined to
 * match. Compile errors are logged. */
typedef struct xd_glfx_shader {
    GLuint program;
    GLint position, texcoord;
    GLint projection, texcoord_scale, texture, alpha;
} xd_glfx_shader;

bool xd_glfx_init_shader(xd_glfx_shader *shader, const char *vertex, const char *fragment,
                         GLenum target);
void xd_glfx_destroy_shader(xd_glfx_shader *shader);

/* ---- meshes --------------------------------------------------------------------- */

/* A grid of columns x rows cells: (columns + 1) x (rows + 1) vertices,
 * row by row from the top left, each with a position the effect sets
 * (layout units) and texture coordinates across the window (0..1, from
 * the top left), drawn as triangles. */
typedef struct xd_glfx_mesh {
    int columns, rows;
    size_t vertex_count, index_count;
    float *positions; /* x, y per vertex */
    float *texcoords; /* u, v per vertex */
    uint16_t *indices;
} xd_glfx_mesh;

/* False past 65535 vertices (GLES2's 16-bit indices). */
bool xd_glfx_init_mesh(xd_glfx_mesh *mesh, int columns, int rows);
void xd_glfx_free_mesh(xd_glfx_mesh *mesh);
static inline size_t xd_glfx_get_mesh_vertex(const xd_glfx_mesh *mesh, int column, int row) {
    return (size_t)row * (size_t)(mesh->columns + 1) + (size_t)column;
}
/* Every vertex where it lies on `box`, undeformed. */
void xd_glfx_set_mesh_rect(xd_glfx_mesh *mesh, struct wlr_fbox box);
/* The box around every vertex. */
struct wlr_fbox xd_glfx_get_mesh_bounds(const xd_glfx_mesh *mesh);

/* ---- drawing -------------------------------------------------------------------- */

typedef struct xd_glfx_texture {
    GLuint id;
    GLenum target; /* GL_TEXTURE_2D or GL_TEXTURE_EXTERNAL_OES */
    int width, height; /* device px of the picture */
    /* The picture fills this much of the texture across and down, from
     * its top left (the buffer is bigger): texture coordinates 0..1 over
     * the picture are multiplied by these. */
    float u_scale, v_scale;
    struct wlr_fbox box; /* what it shows, layout units */
} xd_glfx_texture;

/* What an effect draws into: the buffer behind its bounds. */
typedef struct xd_glfx_draw {
    float projection[16]; /* layout units to the buffer, column-major */
    struct wlr_box bounds; /* layout units the buffer covers */
    double scale;          /* its device px per layout unit */
    struct xd_glfx *glfx;  /* the framework's */
} xd_glfx_draw;

/* Draws the mesh textured with `texture`, faded by `alpha`, with the
 * built-in shader, or `shader` (whose own uniforms are set already). */
void xd_glfx_draw_mesh(const xd_glfx_draw *draw, const xd_glfx_mesh *mesh,
                       const xd_glfx_texture *texture, float alpha);
void xd_glfx_draw_mesh_with(const xd_glfx_draw *draw, const xd_glfx_mesh *mesh,
                            const xd_glfx_texture *texture, const xd_glfx_shader *shader,
                            float alpha);

/* 4 x 4 matrices, column-major as GL takes them. */
void xd_glfx_set_ortho(float out[16], struct wlr_box box);
void xd_glfx_multiply(float out[16], const float left[16], const float right[16]);

/* ---- window effects ------------------------------------------------------------- */

typedef struct xd_window_effect xd_window_effect;

typedef struct xd_window_effect_impl {
    const char *name; /* for the log */
    /* The first frame: `effect->texture` holds the window. Optional. */
    void (*begin)(xd_window_effect *effect);
    /* Advances by `seconds` (since its last frame; 0 the first time);
     * false when it's over, and the window is shown as it is again. */
    bool (*update)(xd_window_effect *effect, double seconds);
    /* Where draw will draw this frame, layout units. */
    struct wlr_fbox (*get_bounds)(xd_window_effect *effect);
    void (*draw)(xd_window_effect *effect, const xd_glfx_draw *draw);
    /* Over, ended, or the window is gone; free `data` here. Optional. */
    void (*done)(xd_window_effect *effect);
} xd_window_effect_impl;

struct xd_window_effect {
    const xd_window_effect_impl *impl;
    xd_view *view;
    void *data;              /* the effect's */
    xd_glfx_texture texture; /* the window, as captured for this frame */
    /* The framework's. */
    struct wl_list link; /* xd_glfx.windows */
    struct wlr_scene_buffer *node;
    struct wlr_swapchain *capture_chain, *target_chain;
    struct wlr_texture *capture_texture;
    struct wlr_buffer *capture_buffer;
    struct wlr_buffer *frame_buffer; /* locked while frame_texture is made from it */
    struct wlr_texture *frame_texture;
    double last_time;
    bool begun;
    /* xd_glfx_begin_closing_window's: the picture taken then, kept; the
     * node beside the window's tree, not in it. */
    bool held;
    double held_scale;
};

/* `impl` takes `view` over from the next frame, with `data`. A window
 * has one effect at a time: an effect already on it is ended first. */
xd_window_effect *xd_glfx_begin_window(xd_view *view, const xd_window_effect_impl *impl,
                                       void *data);
/* A closing effect (close, minimize: Compiz's animations): `view`, about
 * to go (UNMAP) or be hidden (MINIMIZE), is drawn once now into the
 * effect's texture, which it keeps; the effect then runs without the
 * window, its picture stacked where the window was (just above the
 * window's tree, in the same layer), till update says it's over. Call it
 * from the window event: the window's content is still there. Live
 * effects on the window end first. `effect->view` is the window until it
 * is destroyed, nullptr after. nullptr if the window isn't shown or
 * can't be drawn. */
xd_window_effect *xd_glfx_begin_closing_window(xd_view *view, const xd_window_effect_impl *impl,
                                               void *data);
/* The effect `impl` has on `view` (a closing one too), or nullptr. */
xd_window_effect *xd_glfx_find_window(xd_view *view, const xd_window_effect_impl *impl);
/* Ends it now (calls done): the window is drawn as itself again. */
void xd_glfx_end_window(xd_window_effect *effect);
bool xd_glfx_has_window_effects(const xd_server *server);

/* effects.c's: the renderer's name in `support`; false without GLES. */
bool xd_glfx_init(xd_server *server, xd_effects_support *support);
void xd_glfx_finish(xd_server *server);
void xd_glfx_frame(xd_server *server, xd_output *output, double now);
void xd_glfx_frame_done(xd_server *server, xd_output *output);
/* The window is unmapped or destroyed: its effects end (not its closing
 * ones). */
void xd_glfx_forget_view(xd_view *view);
/* The window is destroyed: closing effects let go of it. */
void xd_glfx_drop_view(xd_view *view);

/* ---- output filters ------------------------------------------------------------- */

/* A whole-output effect (Water; a cube, a wall): each output's frame is
 * drawn by the scene graph into a buffer of ours (`scene`), and the filter
 * draws the output's real buffer from it with shaders of its own. Filters
 * run in the order they were added, each drawing from the one before.
 * While any filter is active the output's every frame is drawn (and its
 * scanout is off); one turning idle has the scene repaint the whole
 * output. Outputs that are rotated or flipped aren't filtered. */
typedef struct xd_output_filter xd_output_filter;

typedef struct xd_output_filter_impl {
    const char *name; /* for the log */
    /* Once per frame (all outputs'), `seconds` since the last: whether it
     * changes the picture now. False: idle, drawn as usual, and no frames
     * for it till xd_glfx_wake_output_filter. */
    bool (*update)(xd_output_filter *filter, double seconds);
    /* Draws the whole output from `scene` (`scene->box` is the output's
     * box in layout units, sampled 0..1 across it). `draw->bounds` is the
     * output's box and `draw->projection` maps layout units to it. */
    void (*draw)(xd_output_filter *filter, xd_output *output, const xd_glfx_texture *scene,
                 const xd_glfx_draw *draw);
    /* Removed: free `data` here. Optional. */
    void (*done)(xd_output_filter *filter);
} xd_output_filter_impl;

struct xd_output_filter {
    const xd_output_filter_impl *impl;
    xd_server *server;
    void *data; /* the effect's */
    /* The framework's. */
    struct wl_list link; /* xd_glfx.filters */
    bool active;
    double last_time;
};

xd_output_filter *xd_glfx_add_output_filter(xd_server *server, const xd_output_filter_impl *impl,
                                            void *data);
void xd_glfx_remove_output_filter(xd_output_filter *filter);
/* Its update runs again from the next frame. */
void xd_glfx_wake_output_filter(xd_output_filter *filter);
/* effects.c's, for output.c: whether a filter draws the output's frames,
 * and the frame's state (the scene graph's, filtered). */
bool xd_glfx_is_output_filtered(const xd_output *output);
bool xd_glfx_build_output_state(xd_output *output, struct wlr_scene_output *scene_output,
                                struct wlr_output_state *state);

/* ---- the scene as a texture (glfx_output.c) ---------------------------------------- */

/* What the scene shows of a box, drawn into a texture: the desktop's
 * colour, layer surfaces (the shell's desktop, taskbar, notifications) and
 * windows, bottom to top, as an output there would show them, but with
 * the windows `include_view` picks (nullptr: those shown), whether they're
 * shown or hidden (another desktop's, minimized): what a desktop effect
 * needs to draw desktops that aren't showing, or a window on its own.
 * Never the dragged things and switchers above everything
 * (server->layer_drag), the lock screen while unlocked, nor whole-output
 * effects' own pictures. Buffers drawn fully transparent (a window an
 * OpenGL window effect draws) are left out; their effect's picture is in.
 *
 * Captures happen outside render passes (an effect's `frame` hook, before
 * xd_glfx_draw_output). Each capture object holds its last texture until
 * the next; while any exists, every window's frame keeps its pixels
 * (xd_frame_keep_pixels) for them. */
typedef struct xd_glfx_scene_capture xd_glfx_scene_capture;
typedef bool (*xd_glfx_view_filter)(const xd_view *view, void *data);

xd_glfx_scene_capture *xd_glfx_create_scene_capture(xd_server *server);
void xd_glfx_destroy_scene_capture(xd_glfx_scene_capture *capture);
/* `box` (layout units) at `scale` device px per unit into `out`; false if
 * it couldn't (out unchanged). Layer surfaces are left out unless
 * `layers`, for a window alone. */
bool xd_glfx_capture_scene(xd_glfx_scene_capture *capture, struct wlr_box box, double scale,
                           bool layers, xd_glfx_view_filter include_view, void *data,
                           xd_glfx_texture *out);
/* effects.c: the window is unmapped or destroyed. */
void xd_glfx_capture_forget_view(xd_view *view);

/* ---- whole-output effects (glfx_output.c) ------------------------------------------ */

/* An effect that draws whole outputs, in place of the scene (Desktop Cube,
 * Expo, Shift Switcher): the effect calls xd_glfx_draw_output from its
 * `frame` hook (after capturing what it needs), and `draw` paints the
 * whole output into a buffer of the output's size, which a scene buffer
 * node shows over everything but the lock screen and the dragged things
 * (opaque, so what's under it isn't drawn). Input still goes where it
 * would: an effect taking the pointer uses up its events (effects.h's
 * input hook). The effect drives its own frames
 * (wlr_output_schedule_frame from a timer), as long as it runs. */
typedef struct xd_output_effect xd_output_effect;

typedef struct xd_output_effect_impl {
    const char *name; /* for the log */
    /* `draw->bounds` is the output's layout box. Clip space covers it
     * (xd_glfx_draw_quad); draw->projection maps layout units, too. */
    void (*draw)(xd_output_effect *effect, xd_output *output, const xd_glfx_draw *draw);
    /* Ended: free `data` here. Optional. */
    void (*done)(xd_output_effect *effect);
} xd_output_effect_impl;

struct xd_output_effect {
    const xd_output_effect_impl *impl;
    xd_server *server;
    void *data; /* the effect's */
    struct xd_output_effect_private *private_state; /* the framework's */
};

xd_output_effect *xd_glfx_begin_output(xd_server *server, const xd_output_effect_impl *impl,
                                       void *data);
/* Ends it now (calls done): the scene shows again. */
void xd_glfx_end_output(xd_output_effect *effect);
/* Draws the effect's picture of `output` and shows it. */
void xd_glfx_draw_output(xd_output_effect *effect, xd_output *output);

/* Drawing in 3D, for them: a texture's whole picture on a quad whose
 * corners (top left, top right, bottom right, bottom left) are in clip
 * space with their w (viewport_model.h's xd_camera_project), so the
 * picture is in perspective; faded by `alpha`, its colours times
 * `brightness` (0..1: darker). */
void xd_glfx_draw_quad(const xd_glfx_draw *draw, const xd_glfx_texture *texture,
                       const float corners[4][4], float alpha, float brightness);
/* A convex polygon (clip space, as a fan from the first corner) filled
 * with a colour, RGBA 0..1 not premultiplied. */
void xd_glfx_fill_polygon(const xd_glfx_draw *draw, const float (*corners)[4], size_t count,
                          const float color[4]);

#endif
