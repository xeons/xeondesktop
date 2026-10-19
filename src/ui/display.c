/* SPDX-License-Identifier: MIT
 * libxdui: display configuration through wlr-output-management, for
 * Display Properties' Settings tab.
 */
#include "../core/alloc.h"
#include "ui_internal.h"

/* ---- modes -------------------------------------------------------------------- */

static void mode_size(void *data, [[maybe_unused]] struct zwlr_output_mode_v1 *m, int32_t w,
                      int32_t h) {
    xd_ui_mode *mode = data;
    mode->info.width = w;
    mode->info.height = h;
}

static void mode_refresh(void *data, [[maybe_unused]] struct zwlr_output_mode_v1 *m,
                         int32_t refresh) {
    xd_ui_mode *mode = data;
    mode->info.refresh_mhz = refresh;
}

static void mode_preferred(void *data, [[maybe_unused]] struct zwlr_output_mode_v1 *m) {
    xd_ui_mode *mode = data;
    mode->info.preferred = true;
}

static void free_mode(xd_ui_mode *mode) {
    if (mode->head->current == mode->mode)
        mode->head->current = nullptr;
    wl_list_remove(&mode->link);
    zwlr_output_mode_v1_release(mode->mode);
    xd_free(mode);
}

static void mode_finished(void *data, [[maybe_unused]] struct zwlr_output_mode_v1 *m) {
    free_mode(data);
}

static const struct zwlr_output_mode_v1_listener mode_listener = {
    .size = mode_size,
    .refresh = mode_refresh,
    .preferred = mode_preferred,
    .finished = mode_finished,
};

/* ---- heads -------------------------------------------------------------------- */

static void head_name(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                      const char *name) {
    xd_ui_head *head = data;
    snprintf(head->info.name, sizeof(head->info.name), "%s", name);
}

static void head_description(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                             const char *desc) {
    xd_ui_head *head = data;
    snprintf(head->info.description, sizeof(head->info.description), "%s", desc);
}

static void head_physical_size(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                               int32_t w, int32_t hh) {
    xd_ui_head *head = data;
    head->info.phys_w_mm = w;
    head->info.phys_h_mm = hh;
}

static void head_mode(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                      struct zwlr_output_mode_v1 *m) {
    xd_ui_head *head = data;
    xd_ui_mode *mode = XD_NEW(xd_ui_mode);
    mode->head = head;
    mode->mode = m;
    wl_list_insert(head->modes.prev, &mode->link);
    zwlr_output_mode_v1_add_listener(m, &mode_listener, mode);
}

static void head_enabled(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                         int32_t enabled) {
    xd_ui_head *head = data;
    head->info.enabled = enabled != 0;
    if (!enabled)
        head->current = nullptr;
}

static void head_current_mode(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                              struct zwlr_output_mode_v1 *m) {
    xd_ui_head *head = data;
    head->current = m;
}

static void head_position(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h, int32_t x,
                          int32_t y) {
    xd_ui_head *head = data;
    head->info.x = x;
    head->info.y = y;
}

static void head_transform(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                           int32_t transform) {
    xd_ui_head *head = data;
    /* Flipped transforms aren't offered; show them as their rotation. */
    head->info.transform = (xd_display_transform)(transform & 3);
}

static void head_scale(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                       wl_fixed_t scale) {
    xd_ui_head *head = data;
    head->info.scale = wl_fixed_to_double(scale);
}

static void free_head(xd_ui_head *head) {
    XD_LIST_FOR_EACH_SAFE(mode, tmp, &head->modes, xd_ui_mode, link) {
        free_mode(mode);
    }
    wl_list_remove(&head->link);
    zwlr_output_head_v1_release(head->head);
    xd_free(head->mode_infos);
    xd_free(head);
}

static void head_finished(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h) {
    free_head(data);
}

static void head_make([[maybe_unused]] void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                      [[maybe_unused]] const char *make) {}
static void head_model([[maybe_unused]] void *data,
                       [[maybe_unused]] struct zwlr_output_head_v1 *h,
                       [[maybe_unused]] const char *model) {}
static void head_serial([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwlr_output_head_v1 *h,
                        [[maybe_unused]] const char *serial) {}
static void head_adaptive_sync(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *h,
                               uint32_t state) {
    xd_ui_head *head = data;
    head->info.adaptive_sync = state == ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED;
}

static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name,
    .description = head_description,
    .physical_size = head_physical_size,
    .mode = head_mode,
    .enabled = head_enabled,
    .current_mode = head_current_mode,
    .position = head_position,
    .transform = head_transform,
    .scale = head_scale,
    .finished = head_finished,
    .make = head_make,
    .model = head_model,
    .serial_number = head_serial,
    .adaptive_sync = head_adaptive_sync,
};

/* ---- manager ------------------------------------------------------------------ */

static void manager_head(void *data, [[maybe_unused]] struct zwlr_output_manager_v1 *m,
                         struct zwlr_output_head_v1 *h) {
    xd_ui *ui = data;
    xd_ui_head *head = XD_NEW(xd_ui_head);
    head->head = h;
    head->info.scale = 1.0;
    head->info.mode = -1;
    wl_list_init(&head->modes);
    wl_list_insert(ui->heads.prev, &head->link);
    zwlr_output_head_v1_add_listener(h, &head_listener, head);
}

static void manager_done(void *data, [[maybe_unused]] struct zwlr_output_manager_v1 *m,
                         uint32_t serial) {
    xd_ui *ui = data;
    ui->output_serial = serial;
    if (ui->display_changed != nullptr)
        ui->display_changed(ui->display_changed_user);
}

static void manager_finished(void *data, struct zwlr_output_manager_v1 *m) {
    xd_ui *ui = data;
    zwlr_output_manager_v1_destroy(m);
    ui->output_manager = nullptr;
}

static const struct zwlr_output_manager_v1_listener manager_listener = {
    .head = manager_head,
    .done = manager_done,
    .finished = manager_finished,
};

void xd_display_bind(xd_ui *ui, struct wl_registry *reg, uint32_t name, uint32_t version) {
    if (ui->output_manager != nullptr)
        return;
    ui->output_manager = wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface,
                                          XD_MIN(version, 4u));
    zwlr_output_manager_v1_add_listener(ui->output_manager, &manager_listener, ui);
}

void xd_display_destroy(xd_ui *ui) {
    if (ui->heads.next != nullptr) {
        XD_LIST_FOR_EACH_SAFE(head, tmp, &ui->heads, xd_ui_head, link) {
            free_head(head);
        }
    }
    if (ui->output_manager != nullptr)
        zwlr_output_manager_v1_destroy(ui->output_manager);
    xd_free(ui->head_infos);
}

/* ---- public API --------------------------------------------------------------- */

bool xd_ui_display_heads(xd_ui *ui, const xd_display_head **heads, size_t *count) {
    *heads = nullptr;
    *count = 0;
    if (ui->output_manager == nullptr)
        return false;
    size_t n = (size_t)wl_list_length(&ui->heads);
    ui->head_infos = xd_realloc_array(ui->head_infos, XD_MAX(n, 1), sizeof(*ui->head_infos));
    size_t i = 0;
    XD_LIST_FOR_EACH(head, &ui->heads, xd_ui_head, link) {
        size_t nm = (size_t)wl_list_length(&head->modes);
        head->mode_infos = xd_realloc_array(head->mode_infos, XD_MAX(nm, 1),
                                            sizeof(*head->mode_infos));
        head->info.mode = -1;
        size_t j = 0;
        XD_LIST_FOR_EACH(mode, &head->modes, xd_ui_mode, link) {
            if (mode->mode == head->current)
                head->info.mode = (int)j;
            head->mode_infos[j++] = mode->info;
        }
        head->info.modes = head->mode_infos;
        head->info.mode_count = nm;
        ui->head_infos[i++] = head->info;
    }
    *heads = ui->head_infos;
    *count = n;
    return true;
}

void xd_ui_on_display_change(xd_ui *ui, xd_ui_source_cb cb, void *user) {
    ui->display_changed = cb;
    ui->display_changed_user = user;
}

typedef struct apply_req {
    xd_display_done_cb done;
    void *user;
} apply_req;

static void config_result(apply_req *req, struct zwlr_output_configuration_v1 *c, bool ok) {
    zwlr_output_configuration_v1_destroy(c);
    if (req->done != nullptr)
        req->done(ok, req->user);
    xd_free(req);
}

static void config_succeeded(void *data, struct zwlr_output_configuration_v1 *c) {
    config_result(data, c, true);
}

static void config_failed(void *data, struct zwlr_output_configuration_v1 *c) {
    config_result(data, c, false);
}

static void config_cancelled(void *data, struct zwlr_output_configuration_v1 *c) {
    config_result(data, c, false);
}

static const struct zwlr_output_configuration_v1_listener config_listener = {
    .succeeded = config_succeeded,
    .failed = config_failed,
    .cancelled = config_cancelled,
};

static const xd_display_setting *setting_for(const xd_ui_head *head,
                                             const xd_display_setting *settings, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(settings[i].name, head->info.name) == 0)
            return &settings[i];
    }
    return nullptr;
}

/* Adaptive sync came with version 4 of the configuration head. */
static void set_adaptive_sync(struct zwlr_output_configuration_head_v1 *ch, bool on) {
    if (zwlr_output_configuration_head_v1_get_version(ch) >=
        ZWLR_OUTPUT_CONFIGURATION_HEAD_V1_SET_ADAPTIVE_SYNC_SINCE_VERSION)
        zwlr_output_configuration_head_v1_set_adaptive_sync(
            ch, on ? ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED
                   : ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
}

static void send_config(xd_ui *ui, const xd_display_setting *settings, size_t count,
                        xd_display_done_cb done, void *user, bool test) {
    if (ui->output_manager == nullptr) {
        if (done != nullptr)
            done(false, user);
        return;
    }
    struct zwlr_output_configuration_v1 *c =
        zwlr_output_manager_v1_create_configuration(ui->output_manager, ui->output_serial);
    apply_req *req = XD_NEW(apply_req);
    req->done = done;
    req->user = user;
    zwlr_output_configuration_v1_add_listener(c, &config_listener, req);
    XD_LIST_FOR_EACH(head, &ui->heads, xd_ui_head, link) {
        const xd_display_setting *s = setting_for(head, settings, count);
        bool enabled = s != nullptr ? s->enabled : head->info.enabled;
        if (!enabled) {
            zwlr_output_configuration_v1_disable_head(c, head->head);
            continue;
        }
        struct zwlr_output_configuration_head_v1 *ch =
            zwlr_output_configuration_v1_enable_head(c, head->head);
        if (s == nullptr) {
            /* Unchanged: restate what it has. */
            if (head->current != nullptr)
                zwlr_output_configuration_head_v1_set_mode(ch, head->current);
            zwlr_output_configuration_head_v1_set_position(ch, head->info.x, head->info.y);
            zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(head->info.scale));
            zwlr_output_configuration_head_v1_set_transform(ch, (int32_t)head->info.transform);
            set_adaptive_sync(ch, head->info.adaptive_sync);
            continue;
        }
        int j = 0;
        XD_LIST_FOR_EACH(mode, &head->modes, xd_ui_mode, link) {
            if (j++ == s->mode)
                zwlr_output_configuration_head_v1_set_mode(ch, mode->mode);
        }
        zwlr_output_configuration_head_v1_set_position(ch, s->x, s->y);
        zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(s->scale));
        zwlr_output_configuration_head_v1_set_transform(ch, (int32_t)s->transform);
        set_adaptive_sync(ch, s->adaptive_sync);
    }
    if (test)
        zwlr_output_configuration_v1_test(c);
    else
        zwlr_output_configuration_v1_apply(c);
}

void xd_ui_display_apply(xd_ui *ui, const xd_display_setting *settings, size_t count,
                         xd_display_done_cb done, void *user) {
    send_config(ui, settings, count, done, user, false);
}

void xd_ui_display_test(xd_ui *ui, const xd_display_setting *settings, size_t count,
                        xd_display_done_cb done, void *user) {
    send_config(ui, settings, count, done, user, true);
}
