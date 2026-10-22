/* SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <gphoto2/gphoto2.h>
#include <sane/saneopts.h>
#include <string.h>

#include "devices.h"

void xd_imaging_device_free(xd_imaging_device *imaging_device) {
    if (imaging_device == nullptr)
        return;
    g_free(imaging_device->id);
    g_free(imaging_device->port);
    g_free(imaging_device->vendor);
    g_free(imaging_device->model);
    g_free(imaging_device->type);
    g_free(imaging_device);
}

xd_imaging_device *xd_imaging_device_copy(const xd_imaging_device *original) {
    xd_imaging_device *copy = g_new0(xd_imaging_device, 1);
    copy->camera = original->camera;
    copy->id = g_strdup(original->id);
    copy->port = g_strdup(original->port);
    copy->vendor = g_strdup(original->vendor);
    copy->model = g_strdup(original->model);
    copy->type = g_strdup(original->type);
    return copy;
}

/* ==== SANE ============================================================================ */

static GMutex sane_lock;

/* Call with sane_lock held. */
static bool sane_ready(GError **error) {
    static bool ready = false;
    if (ready)
        return true;
    SANE_Int version = 0;
    SANE_Status st = sane_init(&version, nullptr);
    if (st != SANE_STATUS_GOOD) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "SANE could not be started: %s",
                    sane_strstatus(st));
        return false;
    }
    ready = true;
    return true;
}

static void sane_fail(GError **error, const char *what, SANE_Status st) {
    g_set_error(error, G_IO_ERROR, st == SANE_STATUS_CANCELLED ? G_IO_ERROR_CANCELLED
                                                               : G_IO_ERROR_FAILED,
                "%s: %s", what, sane_strstatus(st));
}

static void list_scanners(GPtrArray *out) {
    g_mutex_lock(&sane_lock);
    const SANE_Device **devs = nullptr;
    if (sane_ready(nullptr) && sane_get_devices(&devs, SANE_FALSE) == SANE_STATUS_GOOD) {
        for (int i = 0; devs[i] != nullptr; i++) {
            xd_imaging_device *imaging_device = g_new0(xd_imaging_device, 1);
            imaging_device->id = g_strdup(devs[i]->name);
            imaging_device->port = g_strdup(devs[i]->name);
            imaging_device->vendor = g_strdup(devs[i]->vendor != nullptr ? devs[i]->vendor : "");
            imaging_device->model =
                g_strdup(devs[i]->model != nullptr ? devs[i]->model : devs[i]->name);
            imaging_device->type = g_strdup(devs[i]->type != nullptr ? devs[i]->type : "scanner");
            g_ptr_array_add(out, imaging_device);
        }
    }
    g_mutex_unlock(&sane_lock);
}

/* The option called `name`, or -1. */
static SANE_Int find_option(SANE_Handle handle, const char *name,
                            const SANE_Option_Descriptor **out) {
    for (SANE_Int i = 1;; i++) {
        const SANE_Option_Descriptor *option = sane_get_option_descriptor(handle, i);
        if (option == nullptr)
            return -1;
        if (option->name != nullptr && strcmp(option->name, name) == 0) {
            *out = option;
            return i;
        }
    }
}

static bool settable(const SANE_Option_Descriptor *option) {
    return SANE_OPTION_IS_ACTIVE(option->cap) && SANE_OPTION_IS_SETTABLE(option->cap);
}

/* Sets a number option (integer or fixed point); quietly nothing when the
 * scanner hasn't the option. */
static void set_number(SANE_Handle handle, const char *name, int value) {
    const SANE_Option_Descriptor *option = nullptr;
    SANE_Int i = find_option(handle, name, &option);
    if (i < 0 || !settable(option))
        return;
    SANE_Word sane_word = option->type == SANE_TYPE_FIXED ? SANE_FIX(value) : value;
    sane_control_option(handle, i, SANE_ACTION_SET_VALUE, &sane_word, nullptr);
}

static void set_bool(SANE_Handle handle, const char *name, bool value) {
    const SANE_Option_Descriptor *option = nullptr;
    SANE_Int i = find_option(handle, name, &option);
    if (i < 0 || !settable(option) || option->type != SANE_TYPE_BOOL)
        return;
    SANE_Bool sane_bool = value ? SANE_TRUE : SANE_FALSE;
    sane_control_option(handle, i, SANE_ACTION_SET_VALUE, &sane_bool, nullptr);
}

static void set_string(SANE_Handle handle, const char *name, const char *value) {
    const SANE_Option_Descriptor *option = nullptr;
    SANE_Int i = find_option(handle, name, &option);
    if (i < 0 || !settable(option) || option->type != SANE_TYPE_STRING || option->size <= 0)
        return;
    char *buf = g_malloc0((gsize)option->size);
    g_strlcpy(buf, value, (gsize)option->size);
    sane_control_option(handle, i, SANE_ACTION_SET_VALUE, buf, nullptr);
    g_free(buf);
}

static int word_dpi(const SANE_Option_Descriptor *option, SANE_Word sane_word) {
    return option->type == SANE_TYPE_FIXED ? (int)SANE_UNFIX(sane_word) : sane_word;
}

/* Reads the scanner's modes and resolutions; call with sane_lock held. */
static xd_scanner_info *read_info(SANE_Handle handle) {
    xd_scanner_info *info = g_new0(xd_scanner_info, 1);
    info->modes = g_ptr_array_new_with_free_func(g_free);
    info->resolutions = g_array_new(FALSE, FALSE, sizeof(int));
    const SANE_Option_Descriptor *option = nullptr;
    if (find_option(handle, SANE_NAME_SCAN_MODE, &option) >= 0 &&
        option->constraint_type == SANE_CONSTRAINT_STRING_LIST) {
        for (const SANE_String_Const *sane_string_const = option->constraint.string_list;
             *sane_string_const != nullptr; sane_string_const++)
            g_ptr_array_add(info->modes, g_strdup(*sane_string_const));
    }
    info->min_dpi = 50;
    info->max_dpi = 600;
    if (find_option(handle, SANE_NAME_SCAN_RESOLUTION, &option) >= 0) {
        if (option->constraint_type == SANE_CONSTRAINT_WORD_LIST) {
            const SANE_Word *list = option->constraint.word_list;
            info->min_dpi = G_MAXINT;
            info->max_dpi = 0;
            for (SANE_Word k = 1; k <= list[0]; k++) {
                int dpi = word_dpi(option, list[k]);
                info->min_dpi = MIN(info->min_dpi, dpi);
                info->max_dpi = MAX(info->max_dpi, dpi);
                if (dpi <= 1200)
                    g_array_append_val(info->resolutions, dpi);
            }
        } else if (option->constraint_type == SANE_CONSTRAINT_RANGE) {
            info->min_dpi = word_dpi(option, option->constraint.range->min);
            info->max_dpi = word_dpi(option, option->constraint.range->max);
            xd_scan_resolutions_in_range(info->min_dpi, info->max_dpi, info->resolutions);
        }
    }
    if (info->resolutions->len == 0) {
        int dpi = 300;
        g_array_append_val(info->resolutions, dpi);
    }
    return info;
}

void xd_scanner_info_free(xd_scanner_info *info) {
    if (info == nullptr)
        return;
    g_ptr_array_unref(info->modes);
    g_array_unref(info->resolutions);
    g_free(info);
}

/* Opens `device` with sane_lock held (released on failure). */
static SANE_Handle open_scanner(const char *device, GError **error) {
    g_mutex_lock(&sane_lock);
    SANE_Handle handle = nullptr;
    if (!sane_ready(error)) {
        g_mutex_unlock(&sane_lock);
        return nullptr;
    }
    SANE_Status st = sane_open(device, &handle);
    if (st != SANE_STATUS_GOOD) {
        sane_fail(error, "The scanner could not be opened", st);
        g_mutex_unlock(&sane_lock);
        return nullptr;
    }
    return handle;
}

static void close_scanner(SANE_Handle handle) {
    sane_close(handle);
    g_mutex_unlock(&sane_lock);
}

xd_scanner_info *xd_scanner_info_get(const char *device, GError **error) {
    SANE_Handle handle = open_scanner(device, error);
    if (handle == nullptr)
        return nullptr;
    xd_scanner_info *info = read_info(handle);
    close_scanner(handle);
    return info;
}

bool xd_scanner_test(const char *device, GError **error) {
    SANE_Handle handle = open_scanner(device, error);
    if (handle == nullptr)
        return false;
    /* It answers about itself and its options: what XP's test checked. */
    SANE_Parameters sane_parameters;
    SANE_Status st = sane_get_parameters(handle, &sane_parameters);
    const SANE_Option_Descriptor *option = sane_get_option_descriptor(handle, 0);
    close_scanner(handle);
    if (st != SANE_STATUS_GOOD) {
        sane_fail(error, "The scanner did not answer", st);
        return false;
    }
    if (option == nullptr) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The scanner did not list its options.");
        return false;
    }
    return true;
}

static void free_pixels(guchar *pixels, [[maybe_unused]] gpointer data) { g_free(pixels); }

/* Reads every frame of the scan into `rgb`, `*width` by `*height`. */
static bool read_scan(SANE_Handle handle, gint *progress, gint *cancel, GByteArray *rgb, int *width,
                      int *height, GError **error) {
    SANE_Parameters sane_parameters;
    bool first = true;
    do {
        SANE_Status st = sane_start(handle);
        if (st != SANE_STATUS_GOOD) {
            sane_fail(error, "The scan could not be started", st);
            return false;
        }
        if ((st = sane_get_parameters(handle, &sane_parameters)) != SANE_STATUS_GOOD) {
            sane_fail(error, "The scanner did not say what it scans", st);
            return false;
        }
        if (first)
            *width = sane_parameters.pixels_per_line;
        first = false;
        if (sane_parameters.pixels_per_line != *width || sane_parameters.bytes_per_line <= 0 ||
            *width <= 0) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "The scanner sent pictures of different sizes.");
            return false;
        }
        size_t bpl = (size_t)sane_parameters.bytes_per_line;
        size_t row = (size_t)*width * 3;
        guint8 *line = g_malloc(bpl);
        size_t filled = 0;
        int y = 0;
        /* A three-pass scanner sends a frame per colour. */
        int frames = sane_parameters.format == SANE_FRAME_RED ||
                             sane_parameters.format == SANE_FRAME_GREEN ||
                             sane_parameters.format == SANE_FRAME_BLUE
                         ? 3
                         : 1;
        int pass = sane_parameters.format == SANE_FRAME_GREEN  ? 1
                   : sane_parameters.format == SANE_FRAME_BLUE ? 2
                                                               : 0;
        for (;;) {
            if (g_atomic_int_get(cancel)) {
                sane_cancel(handle);
                g_free(line);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                    "The scan was cancelled.");
                return false;
            }
            SANE_Int len = 0;
            st = sane_read(handle, line + filled, (SANE_Int)(bpl - filled), &len);
            if (st == SANE_STATUS_EOF)
                break;
            if (st != SANE_STATUS_GOOD) {
                g_free(line);
                sane_fail(error, "The scan failed", st);
                return false;
            }
            filled += (size_t)len;
            if (filled < bpl)
                continue;
            filled = 0;
            if ((size_t)(y + 1) * row > rgb->len) {
                guint old = rgb->len;
                g_byte_array_set_size(rgb, (guint)((size_t)(y + 1) * row));
                memset(rgb->data + old, 0, rgb->len - old);
            }
            if (!xd_scan_convert_line(sane_parameters.format, sane_parameters.depth, *width, line,
                                      rgb->data + (size_t)y * row)) {
                g_free(line);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                    "The scanner sent a picture format this program doesn't "
                                    "read.");
                return false;
            }
            y++;
            if (sane_parameters.lines > 0)
                g_atomic_int_set(
                    progress,
                    (int)(1000 * (pass * sane_parameters.lines + MIN(y, sane_parameters.lines)) /
                          (frames * sane_parameters.lines)));
        }
        g_free(line);
        *height = MAX(*height, y);
    } while (!sane_parameters.last_frame);
    return true;
}

GdkPixbuf *xd_scanner_scan(const char *device, xd_picture_type type, int dpi, bool preview,
                           gint *progress, gint *cancel, GError **error) {
    SANE_Handle handle = open_scanner(device, error);
    if (handle == nullptr)
        return nullptr;
    xd_scanner_info *info = read_info(handle);
    bool one_bit = false;
    const char *mode = xd_scan_mode_for(type, (const char *const *)info->modes->pdata,
                                        info->modes->len, &one_bit);
    if (mode != nullptr)
        set_string(handle, SANE_NAME_SCAN_MODE, mode);
    /* Eight bits a sample is plenty for a PNG; one for black and white
     * where the scanner has no lineart mode. */
    set_number(handle, SANE_NAME_BIT_DEPTH, one_bit ? 1 : 8);
    set_bool(handle, SANE_NAME_PREVIEW, preview);
    set_number(handle, SANE_NAME_SCAN_RESOLUTION,
               preview ? xd_scan_preview_dpi(info->min_dpi, info->max_dpi) : dpi);
    xd_scanner_info_free(info);

    GByteArray *rgb = g_byte_array_new();
    int width = 0, height = 0;
    bool ok = read_scan(handle, progress, cancel, rgb, &width, &height, error);
    sane_cancel(handle); /* ends the scan, as SANE wants after the last frame */
    close_scanner(handle);
    if (ok && height == 0) {
        ok = false;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The scanner sent an empty picture.");
    }
    if (!ok) {
        g_byte_array_unref(rgb);
        return nullptr;
    }
    g_atomic_int_set(progress, 1000);
    g_byte_array_set_size(rgb, (guint)((size_t)width * 3 * (size_t)height));
    guint8 *pixels = g_byte_array_free(rgb, FALSE);
    return gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, FALSE, 8, width, height,
                                    width * 3, free_pixels, nullptr);
}

/* ==== libgphoto2 ======================================================================= */

static void list_cameras(GPtrArray *out) {
    GPContext *ctx = gp_context_new();
    CameraList *list = nullptr;
    if (gp_list_new(&list) < GP_OK) {
        gp_context_unref(ctx);
        return;
    }
    int count = gp_camera_autodetect(list, ctx);
    for (int i = 0; i < count; i++) {
        const char *model = nullptr, *port = nullptr;
        gp_list_get_name(list, i, &model);
        gp_list_get_value(list, i, &port);
        if (model == nullptr || port == nullptr)
            continue;
        xd_imaging_device *imaging_device = g_new0(xd_imaging_device, 1);
        imaging_device->camera = true;
        imaging_device->id = g_strdup(model);
        imaging_device->port = g_strdup(port);
        /* "Canon PowerShot A70": the maker is the first word. */
        const char *space = strchr(model, ' ');
        imaging_device->vendor =
            space != nullptr ? g_strndup(model, (gsize)(space - model)) : g_strdup("");
        imaging_device->model = g_strdup(model);
        imaging_device->type = g_strdup("Digital camera");
        g_ptr_array_add(out, imaging_device);
    }
    gp_list_free(list);
    gp_context_unref(ctx);
}

static int by_name(gconstpointer left, gconstpointer right) {
    const xd_imaging_device *left_device = *(xd_imaging_device *const *)left;
    const xd_imaging_device *right_device = *(xd_imaging_device *const *)right;
    return g_utf8_collate(left_device->model, right_device->model);
}

GPtrArray *xd_imaging_devices_list(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)xd_imaging_device_free);
    list_scanners(out);
    list_cameras(out);
    g_ptr_array_sort(out, by_name);
    /* Two of the same model: "#2", "#3" after the first, as Windows
     * names them. */
    const char *first = nullptr; /* the first of a run of one model, kept as it is */
    unsigned count = 1;
    for (guint i = 0; i < out->len; i++) {
        xd_imaging_device *imaging_device = g_ptr_array_index(out, i);
        if (first == nullptr || strcmp(imaging_device->model, first) != 0) {
            first = imaging_device->model;
            count = 1;
            continue;
        }
        char *numbered = g_strdup_printf("%s #%u", first, ++count);
        g_free(imaging_device->model);
        imaging_device->model = numbered;
    }
    return out;
}

struct xd_camera {
    GMutex lock;
    Camera *camera;
    GPContext *ctx;
};

static bool gp_check(int gp_result, GError **error, const char *what) {
    if (gp_result >= GP_OK)
        return true;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what,
                gp_result_as_string(gp_result));
    return false;
}

xd_camera *xd_camera_open(const char *model, const char *port, GError **error) {
    xd_camera *cam = g_new0(xd_camera, 1);
    g_mutex_init(&cam->lock);
    cam->ctx = gp_context_new();
    CameraAbilitiesList *abilities = nullptr;
    GPPortInfoList *ports = nullptr;
    int gp_result = gp_camera_new(&cam->camera);
    if (gp_result >= GP_OK)
        gp_result = gp_abilities_list_new(&abilities);
    if (gp_result >= GP_OK)
        gp_result = gp_abilities_list_load(abilities, cam->ctx);
    int model_index =
        gp_result >= GP_OK ? gp_abilities_list_lookup_model(abilities, model) : gp_result;
    CameraAbilities camera_abilities;
    if (model_index >= GP_OK && (gp_result = gp_abilities_list_get_abilities(
                                     abilities, model_index, &camera_abilities)) >= GP_OK)
        gp_result = gp_camera_set_abilities(cam->camera, camera_abilities);
    else if (model_index < GP_OK)
        gp_result = model_index;
    if (gp_result >= GP_OK)
        gp_result = gp_port_info_list_new(&ports);
    if (gp_result >= GP_OK)
        gp_result = gp_port_info_list_load(ports);
    int port_index = gp_result >= GP_OK ? gp_port_info_list_lookup_path(ports, port) : gp_result;
    GPPortInfo info;
    if (port_index >= GP_OK &&
        (gp_result = gp_port_info_list_get_info(ports, port_index, &info)) >= GP_OK)
        gp_result = gp_camera_set_port_info(cam->camera, info);
    else if (port_index < GP_OK)
        gp_result = port_index;
    if (gp_result >= GP_OK)
        gp_result = gp_camera_init(cam->camera, cam->ctx);
    if (abilities != nullptr)
        gp_abilities_list_free(abilities);
    if (ports != nullptr)
        gp_port_info_list_free(ports);
    if (!gp_check(gp_result, error, "The camera could not be opened")) {
        xd_camera_close(cam);
        return nullptr;
    }
    return cam;
}

void xd_camera_close(xd_camera *cam) {
    if (cam == nullptr)
        return;
    if (cam->camera != nullptr) {
        gp_camera_exit(cam->camera, cam->ctx);
        gp_camera_unref(cam->camera);
    }
    gp_context_unref(cam->ctx);
    g_mutex_clear(&cam->lock);
    g_free(cam);
}

bool xd_camera_test(const char *model, const char *port, GError **error) {
    xd_camera *cam = xd_camera_open(model, port, error);
    if (cam == nullptr)
        return false;
    CameraText summary;
    int gp_result = gp_camera_get_summary(cam->camera, &summary, cam->ctx);
    xd_camera_close(cam);
    /* Not every driver has a summary; opening was the test. */
    return gp_result >= GP_OK || gp_result == GP_ERROR_NOT_SUPPORTED ||
           gp_check(gp_result, error, "The camera failed");
}

void xd_camera_photo_free(xd_camera_photo *camera_photo) {
    g_free(camera_photo->folder);
    g_free(camera_photo->name);
    if (camera_photo->thumbnail != nullptr)
        g_object_unref(camera_photo->thumbnail);
    g_free(camera_photo);
}

static bool is_picture(const char *name) {
    char *type = g_content_type_guess(name, nullptr, 0, nullptr);
    bool image = g_content_type_is_a(type, "image/*");
    g_free(type);
    return image;
}

/* A thumbnail, about 96 pixels, from the camera's preview of the
 * picture, else the picture itself. */
static GdkPixbuf *thumbnail(xd_camera *cam, const char *folder, const char *name) {
    static const CameraFileType kinds[] = {GP_FILE_TYPE_PREVIEW, GP_FILE_TYPE_NORMAL};
    for (size_t k = 0; k < G_N_ELEMENTS(kinds); k++) {
        CameraFile *file = nullptr;
        if (gp_file_new(&file) < GP_OK)
            return nullptr;
        GdkPixbuf *pix = nullptr;
        const char *data = nullptr;
        unsigned long size = 0;
        if (gp_camera_file_get(cam->camera, folder, name, kinds[k], file, cam->ctx) >= GP_OK &&
            gp_file_get_data_and_size(file, &data, &size) >= GP_OK && size > 0) {
            GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
            if (gdk_pixbuf_loader_write(loader, (const guchar *)data, size, nullptr) &&
                gdk_pixbuf_loader_close(loader, nullptr)) {
                GdkPixbuf *full = gdk_pixbuf_loader_get_pixbuf(loader);
                if (full != nullptr) {
                    /* Kept in proportion: the larger side 96. */
                    int width = gdk_pixbuf_get_width(full), height = gdk_pixbuf_get_height(full);
                    double scale = 96.0 / MAX(width, height);
                    pix =
                        gdk_pixbuf_scale_simple(full, MAX(1, (int)(width * scale)),
                                                MAX(1, (int)(height * scale)), GDK_INTERP_BILINEAR);
                }
            } else {
                gdk_pixbuf_loader_close(loader, nullptr);
            }
            g_object_unref(loader);
        }
        gp_file_unref(file);
        if (pix != nullptr)
            return pix;
    }
    return nullptr;
}

static bool list_folder(xd_camera *cam, const char *folder, GPtrArray *out, gint *cancel,
                        int depth, GError **error) {
    if (depth > 8 || g_atomic_int_get(cancel))
        return true;
    CameraList *list = nullptr;
    gp_list_new(&list);
    int gp_result = gp_camera_folder_list_files(cam->camera, folder, list, cam->ctx);
    if (!gp_check(gp_result, error, "The camera's pictures could not be listed")) {
        gp_list_free(list);
        return false;
    }
    for (int i = 0; i < gp_list_count(list) && !g_atomic_int_get(cancel); i++) {
        const char *name = nullptr;
        gp_list_get_name(list, i, &name);
        if (name == nullptr || !is_picture(name))
            continue;
        xd_camera_photo *camera_photo = g_new0(xd_camera_photo, 1);
        camera_photo->folder = g_strdup(folder);
        camera_photo->name = g_strdup(name);
        camera_photo->thumbnail = thumbnail(cam, folder, name);
        g_ptr_array_add(out, camera_photo);
    }
    gp_list_reset(list);
    bool ok = true;
    if (gp_camera_folder_list_folders(cam->camera, folder, list, cam->ctx) >= GP_OK) {
        for (int i = 0; ok && i < gp_list_count(list); i++) {
            const char *sub = nullptr;
            gp_list_get_name(list, i, &sub);
            if (sub == nullptr)
                continue;
            char *path = g_build_path("/", folder, sub, nullptr);
            ok = list_folder(cam, path, out, cancel, depth + 1, error);
            g_free(path);
        }
    }
    gp_list_free(list);
    return ok;
}

GPtrArray *xd_camera_list_photos(xd_camera *cam, gint *cancel, GError **error) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)xd_camera_photo_free);
    g_mutex_lock(&cam->lock);
    bool ok = list_folder(cam, "/", out, cancel, 0, error);
    g_mutex_unlock(&cam->lock);
    if (!ok) {
        g_ptr_array_unref(out);
        return nullptr;
    }
    return out;
}

bool xd_camera_download(xd_camera *cam, const char *folder, const char *name, const char *dest,
                        GError **error) {
    CameraFile *file = nullptr;
    int gp_result = gp_file_new(&file);
    g_mutex_lock(&cam->lock);
    if (gp_result >= GP_OK)
        gp_result =
            gp_camera_file_get(cam->camera, folder, name, GP_FILE_TYPE_NORMAL, file, cam->ctx);
    g_mutex_unlock(&cam->lock);
    if (gp_result >= GP_OK)
        gp_result = gp_file_save(file, dest);
    if (file != nullptr)
        gp_file_unref(file);
    char *what = g_strdup_printf("%s could not be copied", name);
    bool ok = gp_check(gp_result, error, what);
    g_free(what);
    return ok;
}
