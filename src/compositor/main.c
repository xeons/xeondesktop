/* SPDX-License-Identifier: MIT
 * xd-compositor: Wayland compositor and window manager for XeonDesktop.
 *
 * Runs on a TTY (DRM/KMS via a seat manager) or nested in an existing X11 or
 * Wayland session for development.
 */
#include <dirent.h>
#include <errno.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include <xf86drm.h>

#include "../settings/gtktheme.h"
#include "../settings/inputmethod.h"
#include "../settings/settings.h"
#include "../settings/soundplay.h"
#include "../settings/themepkg.h"
#include "appicons.h"
#include "server.h"

static void usage(FILE *file) {
    fprintf(file, "usage: xd-compositor [options]\n"
                  "  --style classic|luna|whistler  visual style (default classic)\n"
                  "  --scheme FILE                  classic colour scheme (.ini)\n"
                  "  --scale N                      force scale on every output (0.5-4)\n"
                  "  --terminal CMD                 command for Ctrl+Alt+T (default alacritty)\n"
                  "  --shell PATH                   taskbar/shell program (default: xd-shell)\n"
                  "  --no-shell                     don't start the shell\n"
                  "  --allow-virtual-input          let clients inject input (UI tests only)\n"
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
        if (xd_str_eq(arg, XD_STR("--allow-virtual-input"))) {
            cfg->virtual_input = true;
            continue;
        }
        if (xd_str_eq(arg, XD_STR("--no-shell"))) {
            cfg->shell = nullptr;
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
            cfg->cli_style = true;
        } else if (xd_str_eq(arg, XD_STR("--scheme"))) {
            xd_strbuf err = {0};
            bool ok = xd_scheme_load(val, &cfg->scheme, &err);
            if (!ok)
                fprintf(stderr, "%s: %s\n", val, xd_strbuf_cstr(&err));
            xd_strbuf_free(&err);
            if (!ok)
                return false;
            cfg->scheme_path = val;
        } else if (xd_str_eq(arg, XD_STR("--scale"))) {
            if (!xd_str_parse_double(xd_str_from_cstr(val), &cfg->forced_scale) ||
                cfg->forced_scale < 0.5 || cfg->forced_scale > 4) {
                fprintf(stderr, "scale must be between 0.5 and 4\n");
                return false;
            }
        } else if (xd_str_eq(arg, XD_STR("--shell"))) {
            cfg->shell = val;
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

/* Virtual machines' GPUs (VirtualBox and VMware: vmwgfx). */
static bool is_virtual_driver(const char *name) {
    static const char *const drivers[] = {"vmwgfx", "virtio_gpu", "qxl", "vboxvideo"};
    for (size_t i = 0; i < XD_ARRAY_LEN(drivers); i++) {
        if (strcmp(name, drivers[i]) == 0)
            return true;
    }
    return false;
}

/* The compositor's priority, as KWin and Sway set theirs. A display
 * manager may run at background priority (CachyOS's ananicy-cpp types sddm
 * as a "Launcher": nice 16, idle I/O) and the session inherits it, so a
 * busy system delays input and drawing by seconds. Normal I/O, and a raised
 * nice as far as RLIMIT_NICE allows; SCHED_RESET_ON_FORK gives the shell
 * and every program started from it normal priority again. ananicy-cpp's
 * rule for us (data/ananicy) covers systems where the limit forbids it. */
static void set_priority(void) {
    constexpr int WANT = -10;
    struct sched_param param = {0};
    if (sched_setscheduler(0, SCHED_OTHER | SCHED_RESET_ON_FORK, &param) != 0)
        wlr_log_errno(WLR_INFO, "cannot set SCHED_RESET_ON_FORK");
    if (syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_NONE, 0)) !=
        0)
        wlr_log_errno(WLR_INFO, "cannot leave the inherited I/O priority");
    errno = 0;
    int was = getpriority(PRIO_PROCESS, 0);
    if (errno != 0)
        return;
    /* Raising priority (a lower nice) goes as far as 20 - RLIMIT_NICE. */
    struct rlimit lim;
    int floor = 20;
    if (getrlimit(RLIMIT_NICE, &lim) == 0)
        floor = lim.rlim_cur == RLIM_INFINITY ? -20 : 20 - (int)XD_MIN(lim.rlim_cur, (rlim_t)40);
    int now = was;
    if (XD_MAX(WANT, floor) < was) {
        if (setpriority(PRIO_PROCESS, 0, XD_MAX(WANT, floor)) == 0)
            now = XD_MAX(WANT, floor);
        else
            wlr_log_errno(WLR_INFO, "cannot raise priority from nice %d", was);
    }
    if (now != was)
        wlr_log(WLR_INFO, "priority: nice %d (was %d)", now, was);
    if (now > 0)
        wlr_log(WLR_ERROR, "running at background priority (nice %d): input and drawing may lag; "
                "raise RLIMIT_NICE or let ananicy-cpp apply its rule for xd-compositor", now);
}

#ifndef XD_SCENEFX
/* Whether every GPU is a virtual machine's. Their 3D (VirtualBox's in
 * particular) is far slower than drawing in software: 20-50 ms a frame
 * against 1 ms, enough to make the pointer crawl. So there we draw in
 * software, and Aero is opaque (Windows 7 Basic). XD_VM_GPU=1 uses the
 * GPU anyway. */
static bool only_virtual_gpus(void) {
    if (getenv("XD_VM_GPU") != nullptr)
        return false;
    DIR *dir = opendir("/sys/class/drm");
    if (dir == nullptr)
        return false;
    size_t cards = 0;
    bool all_virtual = true;
    struct dirent *dir_entry;
    while ((dir_entry = readdir(dir)) != nullptr) {
        /* card0, not card0-Virtual-1 (a connector). */
        const char *card_name = dir_entry->d_name;
        if (strncmp(card_name, "card", 4) != 0 || card_name[4] == '\0' ||
            strspn(card_name + 4, "0123456789") != strlen(card_name + 4))
            continue;
        char link[512], target[512];
        snprintf(link, sizeof(link), "/sys/class/drm/%s/device/driver", card_name);
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len < 0)
            continue;
        target[len] = '\0';
        const char *slash = strrchr(target, '/');
        cards++;
        all_virtual = all_virtual && is_virtual_driver(slash != nullptr ? slash + 1 : target);
    }
    closedir(dir);
    return cards > 0 && all_virtual;
}
#endif

/* Virtual machines' GPUs with 3D on accept a hardware cursor and show
 * nothing (vmwgfx), as sway users know. Software rendering's cursor
 * works, so only with the GPU renderer (XD_VM_GPU=1).
 * XD_HARDWARE_CURSORS=1 tries the hardware cursor anyway. */
static bool needs_software_cursor(xd_server *server) {
    if (wlr_renderer_is_pixman(server->renderer) || getenv("XD_HARDWARE_CURSORS") != nullptr)
        return false;
    int fd = wlr_backend_get_drm_fd(server->backend);
    if (fd < 0)
        return false;
    drmVersion *drm_version = drmGetVersion(fd);
    if (drm_version == nullptr)
        return false;
    bool virt = is_virtual_driver(drm_version->name);
    if (virt)
        wlr_log(WLR_INFO, "%s with 3D: drawing the cursor in software", drm_version->name);
    drmFreeVersion(drm_version);
    return virt;
}

static bool server_init(xd_server *server) {
    server->display = wl_display_create();
    server->loop = wl_display_get_event_loop(server->display);

    server->backend = wlr_backend_autocreate(server->loop, &server->session);
    if (server->backend == nullptr) {
        wlr_log(WLR_ERROR, "cannot create backend");
        return false;
    }
#ifdef XD_SCENEFX
    /* SceneFX draws with its own GLES2 renderer; without a GPU this build
     * can't run, and main() hands over to the plain xd-compositor. */
    server->renderer = fx_renderer_create(server->backend);
    if (server->renderer == nullptr) {
        wlr_log(WLR_ERROR, "no GPU renderer for SceneFX");
        return false;
    }
#else
    if (getenv("WLR_RENDERER") == nullptr && only_virtual_gpus()) {
        wlr_log(WLR_INFO, "virtual machine GPU: drawing in software (XD_VM_GPU=1 uses the GPU)");
        server->renderer = wlr_pixman_renderer_create();
    } else {
        server->renderer = wlr_renderer_autocreate(server->backend);
    }
#endif
    if (server->renderer == nullptr && getenv("WLR_RENDERER") == nullptr) {
        /* No usable GPU (e.g. a virtual machine without 3D acceleration):
         * draw in software. wlroots never picks pixman by itself. */
        wlr_log(WLR_INFO, "no GPU renderer; falling back to software rendering (pixman)");
        server->renderer = wlr_pixman_renderer_create();
    }
    if (server->renderer == nullptr) {
        wlr_log(WLR_ERROR, "cannot create renderer");
        return false;
    }
    wlr_renderer_init_wl_display(server->renderer, server->display);
    server->software_cursors = needs_software_cursor(server);
    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (server->allocator == nullptr) {
        wlr_log(WLR_ERROR, "cannot create allocator");
        return false;
    }

    server->compositor = wlr_compositor_create(server->display, 6, server->renderer);
    wlr_subcompositor_create(server->display);
    wlr_data_device_manager_create(server->display);

    xd_output_init(server);

    server->scene = wlr_scene_create();
#ifdef XD_SCENEFX
    /* Windows 7's blur: soft and a touch brighter than what's behind. */
    wlr_scene_set_blur_data(server->scene, 3, 5.0f, 0.02f, 0.95f, 0.95f, 1.1f);
    /* Glass is blurred: Aero draws translucent (us, the shell, our apps). */
    setenv("XD_GLASS", "1", 1);
#endif
    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    /* Creation order is stacking order, bottom first. */
    struct wlr_scene_tree *root = &server->scene->tree;
    server->layer_background = wlr_scene_tree_create(root);
    server->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(root);
    server->layers[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(root);
    server->layer_views = wlr_scene_tree_create(root);
    server->layer_on_top = wlr_scene_tree_create(root);
    server->layers[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(root);
    server->layer_fullscreen = wlr_scene_tree_create(root); /* above the taskbar */
    /* X11 menus and tooltips: over the taskbar and fullscreen windows, as on Windows. */
    server->layer_x11_popups = wlr_scene_tree_create(root);
    server->layers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(root);
    server->layer_lock = wlr_scene_tree_create(root); /* screen locker, topmost */
    server->layer_drag = wlr_scene_tree_create(root); /* the dragged thing, over all */

    xd_layers_init(server);
    wl_list_init(&server->zooms);
    xd_view_init(server);
    xd_input_init(server);
    xd_constraints_init(server);
    xd_access_init(server);
    xd_trust_init(server);
    xd_arrange_init(server);
    xd_capture_init(server);
    xd_rectangles_init(server);
    xd_relay_init(server);
    xd_protocols_init(server);
    xd_gamma_init(server);
    xd_present_init(server);
    xd_lock_init(server);
    xd_ime_init(server);
    xd_xwayland_init(server);
    if (server->config.virtual_input)
        xd_input_enable_virtual(server);


    wlr_viewporter_create(server->display);
    wlr_fractional_scale_manager_v1_create(server->display, 1);
    wlr_single_pixel_buffer_manager_v1_create(server->display);
    wlr_presentation_create(server->display, server->backend, 2);
    wlr_xdg_output_manager_v1_create(server->display, server->output_layout);
    /* Screenshot tools, screen recorders, clipboard managers and
     * VirtualBox's shared clipboard (VBoxClient prefers the ext protocol,
     * and falls back to wlr's): any program on the desktop, but no
     * sandboxed one (trust.c). */
    xd_trust_privileged(server, wlr_screencopy_manager_v1_create(server->display)->global);
    xd_trust_privileged(server, wlr_export_dmabuf_manager_v1_create(server->display)->global);
    xd_trust_privileged(server, wlr_data_control_manager_v1_create(server->display)->global);
    xd_trust_privileged(server, wlr_ext_data_control_manager_v1_create(server->display, 1)->global);
    wlr_primary_selection_v1_device_manager_create(server->display);
    /* And the others reaching past a program's own windows. */
    xd_trust_privileged(server, server->ftl_manager->global);
    xd_trust_privileged(server, server->output_manager->global);
    xd_trust_privileged(server, server->layer_shell->global);
    xd_trust_privileged(server, server->virtual_keyboards->global);
    xd_trust_privileged(server, server->ext_toplevels->global);
    xd_trust_privileged(server, server->arranger);
    return true;
}

static void server_finish(xd_server *server) {
    server->terminating = true;
    xd_access_finish(server);
    xd_gamma_finish(server);
    xd_xwayland_finish(server);
    if (server->display != nullptr)
        wl_display_destroy_clients(server->display);
    xd_constraints_finish(server);
    xd_ime_finish(server);
    xd_input_finish(server);
    /* wlroots asserts that nothing still listens to an object it
     * destroys, so detach every server-level listener first. */
    struct wl_listener *listeners[] = {
        &server->new_output,          &server->layout_change,
        &server->new_toplevel,        &server->new_popup,
        &server->new_decoration,      &server->new_kde_decoration, &server->new_layer_surface,
        &server->output_apply,        &server->output_test,        &server->output_power,
        &server->new_idle_inhibitor,  &server->request_activate,   &server->new_lock,
        &server->new_activation_token,
        &server->cursor_motion,       &server->cursor_motion_absolute,
        &server->cursor_button,       &server->cursor_axis,
        &server->cursor_frame,        &server->request_cursor_shape,
        &server->new_input,           &server->request_cursor,
        &server->request_set_selection, &server->new_virtual_keyboard,
        &server->request_set_primary_selection, &server->request_start_drag,
        &server->start_drag,          &server->drag_destroy,
        &server->new_virtual_pointer,
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(listeners); i++)
        xd_unlisten(listeners[i]);
    if (server->sigchld != nullptr)
        wl_event_source_remove(server->sigchld);
    if (server->zooms.next != nullptr)
        xd_animate_finish(server);
    xd_snap_finish(server);
    xd_outline_hide(server);
    if (server->scene != nullptr) {
        /* Destroying the scene frees every node in it, including each
         * output's background; outputs are destroyed later with the
         * backend and must not touch those nodes again. */
        XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
            out->background = nullptr;
        }
        wlr_scene_node_destroy(&server->scene->tree.node);
    }
    if (server->xcursor != nullptr)
        wlr_xcursor_manager_destroy(server->xcursor);
    if (server->cursor != nullptr)
        wlr_cursor_destroy(server->cursor);
    if (server->capture_timeline != nullptr)
        wlr_drm_syncobj_timeline_unref(server->capture_timeline);
    if (server->allocator != nullptr)
        wlr_allocator_destroy(server->allocator);
    if (server->renderer != nullptr)
        wlr_renderer_destroy(server->renderer);
    if (server->backend != nullptr)
        wlr_backend_destroy(server->backend);
    xd_appicons_destroy(server->appicons);
    if (server->display != nullptr)
        wl_display_destroy(server->display);
}

/* Regenerates the GTK3 theme to match the current style and scheme. After
 * a change (`settings`; nullptr at startup), other programs' GTK 3
 * windows follow it too: they're told through GSettings, which only a
 * real session may touch (a nested one's are the host desktop's), and
 * only while ours is the GTK theme in use. Whether the scheme is dark
 * goes there too, for every program that asks the portal (Firefox). */
static void install_gtk_theme(xd_server *server, const xd_settings *settings) {
    xd_strbuf err = {0};
    bool changed = false;
    if (!xd_gtk_theme_install(server->config.style, &server->config.scheme, &changed, &err))
        wlr_log(WLR_ERROR, "cannot write the GTK theme: %s", xd_strbuf_cstr(&err));
    else if (changed && settings != nullptr && server->session != nullptr &&
             settings->gtk_theme[0] == '\0')
        xd_gtk_theme_nudge(nullptr);
    if (server->session != nullptr)
        xd_gtk_theme_set_color_scheme(nullptr, xd_scheme_is_dark(&server->config.scheme));
    xd_strbuf_free(&err);
}

/* Programs started from now on get Display Properties' GTK theme; so do
 * those D-Bus and systemd start for the session, once it's real (see
 * main). */
static void set_gtk_theme_env(const xd_server *server, const xd_settings *settings,
                              bool startup) {
    const char *theme = xd_settings_gtk_theme(settings);
    const char *old = getenv("GTK_THEME");
    if (old != nullptr && strcmp(old, theme) == 0)
        return;
    setenv("GTK_THEME", theme, true);
    if (!startup && server->session != nullptr) {
        static char prog[] = "dbus-update-activation-environment", systemd[] = "--systemd",
                    var[] = "GTK_THEME";
        char *update[] = {prog, systemd, var, nullptr};
        xd_spawn_argv(update);
    }
}

/* Qt programs started from now on follow the GTK theme and get
 * Display Properties' Qt style (see xd_settings_qt_env); so do those D-Bus
 * and systemd start, once the session is real. Running Qt programs can't
 * change style: they keep theirs until restarted. */
static void set_qt_env(const xd_server *server, const xd_settings *settings, bool startup) {
    if (!xd_settings_qt_env(settings) || startup || server->session == nullptr)
        return;
    static char prog[] = "dbus-update-activation-environment", systemd[] = "--systemd",
                style[] = "QT_STYLE_OVERRIDE", palette[] = "QT_GUI_GTK_JSON";
    char *update[5] = {prog, systemd};
    size_t count = 2;
    if (xd_qt_env_ours(style))
        update[count++] = style;
    if (xd_qt_env_ours(palette))
        update[count++] = palette;
    update[count] = nullptr;
    if (count > 2)
        xd_spawn_argv(update);
}

/* The input method framework [Keyboard] chose (ime.c), and the
 * variables programs find it by; those D-Bus and systemd start get them
 * too once the session is real (at startup, with the others in main). */
static void set_ime(xd_server *server, bool startup) {
    if (!xd_ime_apply(server) || startup || server->session == nullptr)
        return;
    static char prog[] = "dbus-update-activation-environment", systemd[] = "--systemd";
    char *update[3 + XD_IM_ENV_VARS] = {prog, systemd};
    size_t count = 2;
    for (size_t i = 0; i < XD_IM_ENV_VARS; i++) {
        if (xd_qt_env_ours(xd_im_env_vars[i]))
            update[count++] = (char *)(uintptr_t)xd_im_env_vars[i];
    }
    update[count] = nullptr;
    if (count > 2)
        xd_spawn_argv(update);
}

/* Title bar icons from the settings' icon theme: a new theme starts a new
 * cache (the callers redraw every frame afterwards). */
static void update_appicons(xd_server *server, const xd_settings *settings) {
    const char *theme = xd_settings_icon_theme(settings);
    if (server->appicons != nullptr && strcmp(xd_appicons_theme(server->appicons), theme) == 0)
        return;
    xd_appicons_destroy(server->appicons);
    server->appicons = xd_appicons_create(server->loop, theme, xd_frame_icon_ready, server);
}

/* Display Properties saved new settings: re-theme everything. */
static int handle_settings_changed([[maybe_unused]] int fd, [[maybe_unused]] uint32_t mask,
                                   void *data) {
    xd_server *server = data;
    if (!xd_settings_watch_changed(server->settings_fd))
        return 0;
    xd_settings settings;
    xd_settings_load(&settings);
    /* From now on the file wins over startup flags, for us and for a
     * restarted shell. */
    server->config.cli_style = false;
    server->config.scheme_path = nullptr;
    server->config.style = settings.style;
    server->config.always_frames = settings.always_frames;
    server->config.animate_windows = settings.animate_windows;
    server->config.menu_transitions = settings.menu_transitions;
    server->config.menu_transition = settings.menu_transition;
    server->config.full_window_drag = settings.full_window_drag;
    const xd_keyboard_settings *current = &server->config.keyboard, *loaded = &settings.keyboard;
    bool ime_changed = strcmp(current->input_method, loaded->input_method) != 0;
    if (strcmp(current->layout, loaded->layout) != 0 ||
        strcmp(current->variant, loaded->variant) != 0 ||
        strcmp(current->options, loaded->options) != 0 ||
        strcmp(current->model, loaded->model) != 0 ||
        current->repeat_delay != loaded->repeat_delay ||
        current->repeat_rate != loaded->repeat_rate) {
        server->config.keyboard = settings.keyboard;
        xd_input_apply_keyboard(server);
    }
    server->config.keyboard.cursor_blink_ms = settings.keyboard.cursor_blink_ms;
    const xd_mouse_settings *ma = &server->config.mouse, *mb = &settings.mouse;
    bool cursor_changed = strcmp(ma->cursor_theme, mb->cursor_theme) != 0 ||
                          ma->cursor_size != mb->cursor_size;
    if (memcmp(ma, mb, sizeof(*ma)) != 0) {
        server->config.mouse = settings.mouse;
        xd_input_apply_mouse(server);
        if (cursor_changed)
            xd_input_apply_cursor_theme(server);
    }
    if (ime_changed) {
        memcpy(server->config.keyboard.input_method, settings.keyboard.input_method,
               sizeof(server->config.keyboard.input_method));
        set_ime(server, false);
    }
    if (memcmp(&server->config.access, &settings.access, sizeof(settings.access)) != 0) {
        server->config.access = settings.access;
        xd_input_apply_access(server);
    }
    xd_settings_scheme(&settings, &server->config.scheme);
    /* A new theme package (or none): everything below is drawn with it,
     * and the frames hold only pixels, so the old one can go after. */
    xd_pkg *old_pkg = xd_settings_use_package(&settings);
    xd_theme_init(&server->logical_theme, server->config.style, &server->config.scheme, 1.0);
    xd_gamma_apply_settings(server, &settings);

    install_gtk_theme(server, &settings);
    set_gtk_theme_env(server, &settings, false);
    set_qt_env(server, &settings, false);
    update_appicons(server, &settings);
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        xd_view_update_decorations(view);
    }
    xd_output_update_backgrounds(server);
    XD_LIST_FOR_EACH(view, &server->views, xd_view, link) {
        xd_view_retheme(view);
    }
    /* Frame sizes may have changed. */
    XD_LIST_FOR_EACH(out, &server->outputs, xd_output, link) {
        xd_view_refit_maximized(server, out);
    }
    xd_pkg_free(old_pkg);
    wlr_log(WLR_INFO, "settings changed: %s style, %s", xd_visual_style_name(settings.style),
            server->config.scheme.name);
    return 0;
}

static int handle_terminate([[maybe_unused]] int sig, void *data) {
    xd_server *server = data;
    server->terminating = true;
    wl_display_terminate(server->display);
    return 0;
}

/* Prefers our program next to this binary (a build tree), else PATH.
 * `buf` must outlive the result. */
static const char *own_program(const char *name, char *buf, size_t size) {
    ssize_t length = readlink("/proc/self/exe", buf, size - 1);
    if (length > 0) {
        buf[length] = '\0';
        char *slash = strrchr(buf, '/');
        size_t len = strlen(name);
        if (slash != nullptr && (size_t)(slash - buf) + len + 2 < size) {
            memcpy(slash + 1, name, len + 1);
            if (access(buf, X_OK) == 0)
                return buf;
        }
    }
    return name;
}

/* The other build of the compositor next to this one, or false: the plain
 * xd-compositor and xd-compositor-fx (SceneFX: Aero's blur) hand over to
 * each other. */
static bool sibling_compositor(const char *name, char *buf, size_t size) {
    ssize_t length = readlink("/proc/self/exe", buf, size - 1);
    if (length <= 0)
        return false;
    buf[length] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash == nullptr)
        return false;
    size_t used = (size_t)(slash + 1 - buf);
    int len = snprintf(slash + 1, size - used, "%s", name);
    if (len < 0 || (size_t)len >= size - used)
        return false;
    return access(buf, X_OK) == 0;
}

static void hand_over(const char *name, char **argv) {
    char path[4096];
    if (!sibling_compositor(name, path, sizeof(path)))
        return;
    argv[0] = path;
    execv(path, argv);
    wlr_log_errno(WLR_ERROR, "cannot run %s", path);
}

int main(int argc, char **argv) {
#ifndef XD_SCENEFX
    /* With a GPU, the SceneFX build draws Aero's blurred glass; it hands
     * back (XD_NO_FX set) when there's no GPU renderer. Not when software
     * rendering was asked for, or on a virtual machine. */
    const char *want = getenv("WLR_RENDERER");
    if (getenv("XD_NO_FX") == nullptr && (want == nullptr || strcmp(want, "gles2") == 0) &&
        !only_virtual_gpus())
        hand_over("xd-compositor-fx", argv);
#endif
    static xd_server server;
    server.config.style = XD_STYLE_CLASSIC;
    server.config.terminal = "alacritty";
    static char shell_path[4096], taskmgr_path[4096], locker_path[4096], polkit_path[4096],
        security_path[4096];
    server.config.shell = own_program("xd-shell", shell_path, sizeof(shell_path));
    server.config.taskmgr = own_program("xd-taskmgr", taskmgr_path, sizeof(taskmgr_path));
    server.config.locker = own_program("xd-lock", locker_path, sizeof(locker_path));
    server.config.security = own_program("xd-security", security_path, sizeof(security_path));
    server.config.polkit = own_program("xd-polkit", polkit_path, sizeof(polkit_path));
    static char endtask_path[4096];
    server.config.endtask = own_program("xd-endtask", endtask_path, sizeof(endtask_path));
    static char vboxdnd_path[4096];
    server.config.vboxdnd = own_program("xd-vboxdnd", vboxdnd_path, sizeof(vboxdnd_path));
    static char snip_path[4096];
    server.config.snip = own_program("xd-snip", snip_path, sizeof(snip_path));
    static char access_path[4096];
    server.config.accessibility = own_program("xd-access", access_path, sizeof(access_path));
    xd_scheme_init_default(&server.config.scheme);

    bool debug = false;
    if (!parse_args(argc, argv, &server.config, &debug)) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, nullptr);
    set_priority();

    /* desktop.ini, unless overridden on the command line. */
    xd_settings settings;
    xd_settings_load(&settings);
    if (!server.config.cli_style)
        server.config.style = settings.style;
    server.config.always_frames = settings.always_frames;
    server.config.animate_windows = settings.animate_windows;
    server.config.menu_transitions = settings.menu_transitions;
    server.config.menu_transition = settings.menu_transition;
    server.config.full_window_drag = settings.full_window_drag;
    server.config.keyboard = settings.keyboard;
    server.config.mouse = settings.mouse;
    server.config.access = settings.access;
    if (server.config.scheme_path == nullptr)
        xd_settings_scheme(&settings, &server.config.scheme);
    xd_pkg_free(xd_settings_use_package(&settings)); /* none before it */
    xd_theme_init(&server.logical_theme, server.config.style, &server.config.scheme, 1.0);

    if (!server_init(&server)) {
#ifdef XD_SCENEFX
        bool no_gpu = server.renderer == nullptr;
#endif
        server_finish(&server);
#ifdef XD_SCENEFX
        /* No GPU: the plain build draws with pixman (and without blur). */
        if (no_gpu) {
            setenv("XD_NO_FX", "1", 1);
            unsetenv("XD_GLASS");
            hand_over("xd-compositor", argv);
        }
#endif
        return EXIT_FAILURE;
    }
    /* Before the outputs appear, so they start with their ramps. */
    xd_gamma_apply_settings(&server, &settings);

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

    /* Log Off (xd-shutdown) ends the session with SIGTERM. */
    struct wl_event_source *sigterm =
        wl_event_loop_add_signal(server.loop, SIGTERM, handle_terminate, &server);
    struct wl_event_source *sigint =
        wl_event_loop_add_signal(server.loop, SIGINT, handle_terminate, &server);

    /* Tell session programs how to reach us, and whether this is a test or
     * nested session (where Shut Down must not power off the machine and
     * the user's D-Bus environment must be left alone): only a session on
     * a seat (DRM and libinput, through logind or seatd) is real. Headless
     * test sessions are started without WAYLAND_DISPLAY or DISPLAY, so
     * those can't tell. */
    bool nested = server.session == nullptr;
    char pid[32];
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    setenv("XEONDESKTOP_PID", pid, true);
    setenv("XEONDESKTOP_NESTED", nested ? "1" : "0", true);
    setenv("WAYLAND_DISPLAY", socket, true);
    /* Children are clients of this compositor, not of the host: X11 apps
     * go to our Xwayland. */
    if (server.xwayland != nullptr)
        setenv("DISPLAY", server.xwayland->display_name, true);
    else
        unsetenv("DISPLAY");
    setenv("XDG_CURRENT_DESKTOP", "XeonDesktop", true);
    /* GTK3 apps use the theme generated from our appearance settings (or
     * the one chosen in Display Properties), with Windows-style permanent
     * scroll bars. */
    install_gtk_theme(&server, nullptr);
    set_gtk_theme_env(&server, &settings, true);
    setenv("GTK_OVERLAY_SCROLLING", "0", true);
    /* Qt programs follow the GTK theme, unless the user set up their own
     * (qt6ct, Kvantum, ...) in the environment. */
    set_qt_env(&server, &settings, true);
    /* The input method, for X11 programs (XIM) and Qt. */
    set_ime(&server, true);
    /* Programs that D-Bus and systemd start for the session (portals,
     * notification daemons, scheduled tasks, ...) need to find this
     * display too. Once the user manager has the variables, starting
     * xeondesktop-session.target starts graphical-session.target and what
     * waits for it (Scheduled Tasks' "When I log on"), as other wlroots
     * desktops do; xeondesktop-session stops it when we exit. */
    if (!nested) {
        static char sh[] = "/bin/sh", flag[] = "-c",
                    script[] = "dbus-update-activation-environment --systemd \"$@\" && "
                               "exec systemctl --user start xeondesktop-session.target",
                    arg0[] = "xd-session-start", v1[] = "WAYLAND_DISPLAY", v2[] = "DISPLAY",
                    v3[] = "XDG_CURRENT_DESKTOP", v4[] = "XDG_SESSION_TYPE",
                    v5[] = "XDG_SESSION_DESKTOP", v6[] = "DBUS_SESSION_BUS_ADDRESS",
                    v7[] = "GTK_THEME", v8[] = "XEONDESKTOP_PID", v9[] = "XEONDESKTOP_NESTED",
                    qt1[] = "QT_QPA_PLATFORMTHEME", qt2[] = "QT_STYLE_OVERRIDE",
                    qt3[] = "QT_GUI_GTK_JSON";
        char *update[16 + XD_IM_ENV_VARS] = {sh, flag, script, arg0, v1, v2, v3,
                                             v4, v5, v6, v7, v8, v9};
        size_t count = 13;
        /* The Qt variables only if they're ours: the user's own setup
         * (environment.d) is the user manager's already. */
        if (xd_qt_env_ours(qt1))
            update[count++] = qt1;
        if (xd_qt_env_ours(qt2))
            update[count++] = qt2;
        if (xd_qt_env_ours(qt3))
            update[count++] = qt3;
        for (size_t i = 0; i < XD_IM_ENV_VARS; i++) {
            if (xd_qt_env_ours(xd_im_env_vars[i]))
                update[count++] = (char *)(uintptr_t)xd_im_env_vars[i];
        }
        update[count] = nullptr;
        xd_spawn_argv(update);
    }
    wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s (%s style, %s)", socket,
            xd_visual_style_name(server.config.style), server.config.scheme.name);

    /* The shell's icon theme, for title bars. */
    update_appicons(&server, &settings);

    server.settings_fd = xd_settings_watch();
    if (server.settings_fd >= 0)
        server.settings_watch = wl_event_loop_add_fd(server.loop, server.settings_fd,
                                                     WL_EVENT_READABLE, handle_settings_changed,
                                                     &server);

    xd_shell_start(&server);
    /* Password prompts for administrator actions. In a nested session the
     * host desktop's agent usually has the session already; ours then
     * just exits. Restarted if it crashes, like the shell. */
    xd_polkit_start(&server);
    xd_relay_start(&server);
    if (server.config.startup != nullptr)
        xd_spawn(&server, server.config.startup);

    wl_display_run(server.display);
    wl_event_source_remove(sigterm);
    wl_event_source_remove(sigint);
    if (server.settings_watch != nullptr)
        wl_event_source_remove(server.settings_watch);
    if (server.settings_fd >= 0)
        close(server.settings_fd);
    server_finish(&server);
    xd_sound_finish();
    return EXIT_SUCCESS;
}
