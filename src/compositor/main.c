/* SPDX-License-Identifier: MIT
 * xd-compositor: Wayland compositor and window manager for XeonDesktop.
 *
 * Runs on a TTY (DRM/KMS via a seat manager) or nested in an existing X11 or
 * Wayland session for development.
 */
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>

#include "server.h"

static void usage(FILE *f) {
    fprintf(f, "usage: xd-compositor [options]\n"
               "  --style classic|luna|whistler  visual style (default classic)\n"
               "  --scheme FILE                  classic colour scheme (.ini)\n"
               "  --scale N                      force scale on every output (0.5-4)\n"
               "  --terminal CMD                 command for Ctrl+Alt+T (default alacritty)\n"
               "  -s, --startup CMD              run CMD once the compositor is up\n"
               "  -d, --debug                    verbose logging\n");
}

static bool parse_args(int argc, char **argv, xd_config *cfg, bool *debug) {
    for (int i = 1; i < argc; i++) {
        xd_str arg = xd_str_from_cstr(argv[i]);
        if (xd_str_eq(arg, XD_STR("-h")) || xd_str_eq(arg, XD_STR("--help"))) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        }
        if (xd_str_eq(arg, XD_STR("-d")) || xd_str_eq(arg, XD_STR("--debug"))) {
            *debug = true;
            continue;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return false;
        }
        const char *val = argv[++i];
        if (xd_str_eq(arg, XD_STR("--style"))) {
            if (!xd_visual_style_from_name(xd_str_from_cstr(val), &cfg->style)) {
                fprintf(stderr, "unknown style: %s\n", val);
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            xd_strbuf err = {0};
            bool ok = xd_scheme_load(val, &cfg->scheme, &err);
            if (!ok)
                fprintf(stderr, "%s: %s\n", val, xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            if (!ok)
                return false;
        } else if (xd_str_eq(arg, XD_STR("--scale"))) {
            if (!xd_str_parse_double(xd_str_from_cstr(val), &cfg->forced_scale) ||
                cfg->forced_scale < 0.5 || cfg->forced_scale > 4) {
                fprintf(stderr, "scale must be between 0.5 and 4\n");
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--terminal"))) {
            cfg->terminal = val;
        } else if (xd_str_eq(arg, XD_STR("-s")) || xd_str_eq(arg, XD_STR("--startup"))) {
            cfg->startup = val;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i - 1]);
            return false;
        }
    }
    return true;
}

static bool server_init(xd_server *server) {
    server->display = wl_display_create();
    server->loop = wl_display_get_event_loop(server->display);

    server->backend = wlr_backend_autocreate(server->loop, &server->session);
    if (server->backend == nullptr) {
        wlr_log(WLR_ERROR, "cannot create backend");
        return false;
    }
    server->renderer = wlr_renderer_autocreate(server->backend);
    if (server->renderer == nullptr) {
        wlr_log(WLR_ERROR, "cannot create renderer");
        return false;
    }
    wlr_renderer_init_wl_display(server->renderer, server->display);
    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (server->allocator == nullptr) {
        wlr_log(WLR_ERROR, "cannot create allocator");
        return false;
    }

    wlr_compositor_create(server->display, 6, server->renderer);
    wlr_subcompositor_create(server->display);
    wlr_data_device_manager_create(server->display);

    xd_output_init(server);

    server->scene = wlr_scene_create();
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    server->layer_background = wlr_scene_tree_create(&server->scene->tree);
    server->layer_views = wlr_scene_tree_create(&server->scene->tree);

    xd_view_init(server);
    xd_input_init(server);

    /* GTK uses the KDE protocol to ask for server-side decorations. */
    struct wlr_server_decoration_manager *kde = wlr_server_decoration_manager_create(server->display);
    wlr_server_decoration_manager_set_default_mode(kde, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

    wlr_viewporter_create(server->display);
    wlr_fractional_scale_manager_v1_create(server->display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->display);
    wlr_presentation_create(server->display, server->backend, 2);
    wlr_xdg_output_manager_v1_create(server->display, server->output_layout);
    wlr_screencopy_manager_v1_create(server->display);
    wlr_export_dmabuf_manager_v1_create(server->display);
    wlr_data_control_manager_v1_create(server->display);
    wlr_primary_selection_v1_device_manager_create(server->display);
    return true;
}

static void server_finish(xd_server *server) {
    if (server->display != nullptr)
        wl_display_destroy_clients(server->display);
    if (server->sigchld != nullptr)
        wl_event_source_remove(server->sigchld);
    if (server->scene != nullptr)
        wlr_scene_node_destroy(&server->scene->tree.node);
    if (server->xcursor != nullptr)
        wlr_xcursor_manager_destroy(server->xcursor);
    if (server->cursor != nullptr)
        wlr_cursor_destroy(server->cursor);
    if (server->allocator != nullptr)
        wlr_allocator_destroy(server->allocator);
    if (server->renderer != nullptr)
        wlr_renderer_destroy(server->renderer);
    if (server->backend != nullptr)
        wlr_backend_destroy(server->backend);
    if (server->display != nullptr)
        wl_display_destroy(server->display);
}

int main(int argc, char **argv) {
    static xd_server server;
    server.config.style = XD_STYLE_CLASSIC;
    server.config.terminal = "alacritty";
    xd_scheme_init_default(&server.config.scheme);

    bool debug = false;
    if (!parse_args(argc, argv, &server.config, &debug)) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, nullptr);
    xd_theme_init(&server.logical_theme, server.config.style, &server.config.scheme, 1.0);

    if (!server_init(&server)) {
        server_finish(&server);
        return EXIT_FAILURE;
    }

    const char *socket = wl_display_add_socket_auto(server.display);
    if (socket == nullptr) {
        wlr_log(WLR_ERROR, "cannot create Wayland socket");
        server_finish(&server);
        return EXIT_FAILURE;
    }
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "cannot start backend");
        server_finish(&server);
        return EXIT_FAILURE;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    /* Children are Wayland clients of this compositor, not of the host. */
    unsetenv("DISPLAY");
    setenv("XDG_CURRENT_DESKTOP", "XeonDesktop", true);
    wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s (%s style, %s)", socket,
            xd_visual_style_name(server.config.style), server.config.scheme.name);

    if (server.config.startup != nullptr)
        xd_spawn(&server, server.config.startup);

    wl_display_run(server.display);
    server_finish(&server);
    return EXIT_SUCCESS;
}
