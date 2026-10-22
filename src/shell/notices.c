/* SPDX-License-Identifier: MIT
 * Notifications as kept. See notices.h.
 */
#include "notices.h"

#include <gio/gio.h>
#include <string.h>

#include "../core/alloc.h"

/* What's kept of a notification: enough for a balloon and the history,
 * not whatever a program sends. */
static constexpr glong APP_NAME_MAX = 128; /* characters */
static constexpr glong SUMMARY_MAX = 256;
static constexpr glong BODY_MAX = 2048;
static constexpr glong LABEL_MAX = 64;
static constexpr size_t ACTIONS_MAX = 8; /* pairs */
static constexpr int32_t IMAGE_SIDE_MAX = 1024;

/* Valid UTF-8, at most `max` characters. */
static char *clean_text(const char *text, glong max) {
    char *valid = g_utf8_make_valid(text != nullptr ? text : "", -1);
    if (g_utf8_strlen(valid, -1) <= max)
        return valid;
    char *cut = g_utf8_substring(valid, 0, max);
    g_free(valid);
    return cut;
}

/* ---- markup ------------------------------------------------------------------------- */

static void markup_start([[maybe_unused]] GMarkupParseContext *context, const char *element,
                         const char **attribute_names, const char **attribute_values,
                         gpointer data, [[maybe_unused]] GError **error) {
    GString *out = data;
    if (strcmp(element, "img") != 0)
        return;
    /* A picture is its description. */
    for (size_t i = 0; attribute_names[i] != nullptr; i++) {
        if (strcmp(attribute_names[i], "alt") == 0)
            g_string_append(out, attribute_values[i]);
    }
}

static void markup_text([[maybe_unused]] GMarkupParseContext *context, const char *text,
                        gsize length, gpointer data, [[maybe_unused]] GError **error) {
    g_string_append_len(data, text, (gssize)length);
}

char *xd_notice_plain_text(const char *body) {
    if (body == nullptr)
        return g_strdup("");
    if (strchr(body, '<') == nullptr && strchr(body, '&') == nullptr)
        return g_strdup(body);
    static const GMarkupParser parser = {
        .start_element = markup_start,
        .text = markup_text,
    };
    GString *out = g_string_new(nullptr);
    GMarkupParseContext *context = g_markup_parse_context_new(&parser, 0, out, nullptr);
    /* Inside an element of our own: the body is a fragment. */
    bool ok = g_markup_parse_context_parse(context, "<markup>", -1, nullptr) &&
              g_markup_parse_context_parse(context, body, -1, nullptr) &&
              g_markup_parse_context_parse(context, "</markup>", -1, nullptr) &&
              g_markup_parse_context_end_parse(context, nullptr);
    g_markup_parse_context_free(context);
    if (!ok) {
        /* Not markup after all ("Tom & Jerry", "a < b"): as it is. */
        g_string_free(out, TRUE);
        return g_strdup(body);
    }
    return g_string_free(out, FALSE);
}

/* ---- pictures ----------------------------------------------------------------------- */

/* image-data's "(iiibiiay)": width, height, rowstride, has alpha, bits per
 * sample, channels, the pixels (RGB or RGBA rows). nullptr for nonsense. */
static cairo_surface_t *image_from_variant(GVariant *value) {
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE("(iiibiiay)")))
        return nullptr;
    int32_t width = 0, height = 0, rowstride = 0, bits = 0, channels = 0;
    gboolean alpha = FALSE;
    GVariant *pixels_value = nullptr;
    g_variant_get(value, "(iiibii@ay)", &width, &height, &rowstride, &alpha, &bits, &channels,
                  &pixels_value);
    gsize length = 0;
    const uint8_t *pixels = g_variant_get_fixed_array(pixels_value, &length, 1);
    cairo_surface_t *image = nullptr;
    bool sane = width > 0 && height > 0 && width <= IMAGE_SIDE_MAX && height <= IMAGE_SIDE_MAX &&
                bits == 8 && channels == (alpha ? 4 : 3) && rowstride >= width * channels &&
                (gsize)rowstride * (gsize)(height - 1) + (gsize)(width * channels) <= length;
    if (sane)
        image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (image != nullptr && cairo_surface_status(image) == CAIRO_STATUS_SUCCESS) {
        cairo_surface_flush(image);
        uint8_t *out = cairo_image_surface_get_data(image);
        int out_stride = cairo_image_surface_get_stride(image);
        for (int32_t row = 0; row < height; row++) {
            const uint8_t *in = pixels + (gsize)row * (gsize)rowstride;
            uint32_t *line = (uint32_t *)(void *)(out + (gsize)row * (gsize)out_stride);
            for (int32_t column = 0; column < width; column++) {
                const uint8_t *pixel = in + (gsize)column * (gsize)channels;
                uint32_t opacity = alpha ? pixel[3] : 255;
                /* Cairo's premultiplied native-endian ARGB. */
                uint32_t red = pixel[0] * opacity / 255, green = pixel[1] * opacity / 255,
                         blue = pixel[2] * opacity / 255;
                line[column] = opacity << 24 | red << 16 | green << 8 | blue;
            }
        }
        cairo_surface_mark_dirty(image);
    } else if (image != nullptr) {
        cairo_surface_destroy(image);
        image = nullptr;
    }
    g_variant_unref(pixels_value);
    return image;
}

/* image-path or app_icon: a file (path or file: URI) or a theme icon's
 * name; nullptr for anything else. */
static char *icon_of(const char *text) {
    if (text == nullptr || text[0] == '\0' || !g_utf8_validate(text, -1, nullptr))
        return nullptr;
    if (g_str_has_prefix(text, "file://"))
        return g_filename_from_uri(text, nullptr, nullptr);
    if (text[0] == '/')
        return g_strdup(text);
    if (strchr(text, '/') != nullptr || strlen(text) > 255)
        return nullptr;
    return g_strdup(text);
}

/* ---- a notice ----------------------------------------------------------------------- */

void xd_notice_free(xd_notice *notice) {
    if (notice == nullptr)
        return;
    g_free(notice->sender);
    g_free(notice->app_name);
    g_free(notice->desktop_entry);
    g_free(notice->icon);
    if (notice->image != nullptr)
        cairo_surface_destroy(notice->image);
    g_free(notice->summary);
    g_free(notice->body);
    g_strfreev(notice->actions);
    xd_free(notice);
}

static char **parse_actions(GVariant *array) {
    GPtrArray *out = g_ptr_array_new();
    gsize count = g_variant_n_children(array);
    for (gsize i = 0; i + 1 < count && out->len / 2 < ACTIONS_MAX; i += 2) {
        const char *key = nullptr, *label = nullptr;
        g_variant_get_child(array, i, "&s", &key);
        g_variant_get_child(array, i + 1, "&s", &label);
        if (key[0] == '\0' || !g_utf8_validate(key, -1, nullptr) || strlen(key) > 128)
            continue;
        g_ptr_array_add(out, g_strdup(key));
        g_ptr_array_add(out, clean_text(label, LABEL_MAX));
    }
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

/* desktop-entry: an id ("org.gnome.Nautilus"), as a file name. */
static char *desktop_entry_of(const char *text) {
    if (text == nullptr || text[0] == '\0' || strchr(text, '/') != nullptr ||
        !g_utf8_validate(text, -1, nullptr) || strlen(text) > 200)
        return nullptr;
    return g_str_has_suffix(text, ".desktop") ? g_strdup(text)
                                              : g_strconcat(text, ".desktop", nullptr);
}

static void parse_hints(xd_notice *notice, GVariant *hints, const char *app_icon) {
    GVariant *value = nullptr;
    char *image_path = nullptr;
    GVariant *icon_data = nullptr;
    GVariantIter iter;
    const char *key = nullptr;
    g_variant_iter_init(&iter, hints);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        bool is_string = g_variant_is_of_type(value, G_VARIANT_TYPE_STRING);
        bool is_bool = g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN);
        if (strcmp(key, "urgency") == 0 && g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) {
            uint8_t level = g_variant_get_byte(value);
            notice->urgency = level >= XD_NOTICE_CRITICAL ? XD_NOTICE_CRITICAL
                              : level == XD_NOTICE_LOW    ? XD_NOTICE_LOW
                                                          : XD_NOTICE_NORMAL;
        } else if (strcmp(key, "transient") == 0 && is_bool) {
            notice->transient = g_variant_get_boolean(value);
        } else if (strcmp(key, "resident") == 0 && is_bool) {
            notice->resident = g_variant_get_boolean(value);
        } else if (strcmp(key, "suppress-sound") == 0 && is_bool) {
            notice->suppress_sound = g_variant_get_boolean(value);
        } else if (strcmp(key, "desktop-entry") == 0 && is_string) {
            g_free(notice->desktop_entry);
            notice->desktop_entry = desktop_entry_of(g_variant_get_string(value, nullptr));
        } else if ((strcmp(key, "image-data") == 0 || strcmp(key, "image_data") == 0) &&
                   notice->image == nullptr) {
            notice->image = image_from_variant(value);
        } else if (strcmp(key, "icon_data") == 0 && icon_data == nullptr) {
            icon_data = g_variant_ref(value);
        } else if ((strcmp(key, "image-path") == 0 || strcmp(key, "image_path") == 0) &&
                   is_string && image_path == nullptr) {
            image_path = icon_of(g_variant_get_string(value, nullptr));
        }
        g_variant_unref(value);
    }
    /* The spec's order: image-data, image-path, app_icon, icon_data. */
    notice->icon = image_path != nullptr ? image_path : icon_of(app_icon);
    if (notice->image == nullptr && notice->icon == nullptr && icon_data != nullptr)
        notice->image = image_from_variant(icon_data);
    if (icon_data != nullptr)
        g_variant_unref(icon_data);
}

xd_notice *xd_notice_parse(GVariant *parameters, const char *sender, uint32_t *replaces_id,
                           GError **error) {
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(susssasa{sv}i)"))) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Notify takes (susssasa{sv}i)");
        return nullptr;
    }
    const char *app_name = nullptr, *app_icon = nullptr, *summary = nullptr, *body = nullptr;
    GVariant *actions = nullptr, *hints = nullptr;
    int32_t expire_ms = -1;
    g_variant_get(parameters, "(&su&s&s&s@as@a{sv}i)", &app_name, replaces_id, &app_icon,
                  &summary, &body, &actions, &hints, &expire_ms);
    xd_notice *notice = XD_NEW(xd_notice);
    notice->sender = g_strdup(sender);
    notice->app_name = clean_text(app_name, APP_NAME_MAX);
    notice->summary = clean_text(summary, SUMMARY_MAX);
    char *plain = xd_notice_plain_text(body);
    notice->body = clean_text(plain, BODY_MAX);
    g_free(plain);
    notice->actions = parse_actions(actions);
    notice->urgency = XD_NOTICE_NORMAL;
    notice->expire_ms = expire_ms < -1 ? -1 : expire_ms;
    notice->time = g_get_real_time() / G_USEC_PER_SEC;
    parse_hints(notice, hints, app_icon);
    g_variant_unref(actions);
    g_variant_unref(hints);
    return notice;
}

const char *xd_notice_action_label(const xd_notice *notice, const char *key) {
    for (size_t i = 0; notice->actions[i] != nullptr && notice->actions[i + 1] != nullptr;
         i += 2) {
        if (strcmp(notice->actions[i], key) == 0)
            return notice->actions[i + 1];
    }
    return nullptr;
}

size_t xd_notice_button_count(const xd_notice *notice) {
    size_t count = 0;
    for (size_t i = 0; notice->actions[i] != nullptr && notice->actions[i + 1] != nullptr;
         i += 2) {
        if (strcmp(notice->actions[i], "default") != 0)
            count++;
    }
    return count;
}

const char *xd_notice_button_key(const xd_notice *notice, size_t index) {
    size_t count = 0;
    for (size_t i = 0; notice->actions[i] != nullptr && notice->actions[i + 1] != nullptr;
         i += 2) {
        if (strcmp(notice->actions[i], "default") == 0)
            continue;
        if (count++ == index)
            return notice->actions[i];
    }
    return nullptr;
}

/* ---- the ones kept ------------------------------------------------------------------- */

void xd_notices_init(xd_notices *notices) {
    notices->list = g_ptr_array_new_with_free_func((GDestroyNotify)xd_notice_free);
    notices->last_id = 0;
}

void xd_notices_finish(xd_notices *notices) {
    if (notices->list != nullptr)
        g_ptr_array_unref(notices->list);
    notices->list = nullptr;
}

static uint32_t next_id(xd_notices *notices) {
    /* 0 means none; the ids wrap after four billion. */
    do {
        notices->last_id++;
    } while (notices->last_id == 0 || xd_notices_find(notices, notices->last_id) != nullptr);
    return notices->last_id;
}

static bool index_of(const xd_notices *notices, uint32_t id, guint *index) {
    for (guint i = 0; i < notices->list->len; i++) {
        const xd_notice *kept = g_ptr_array_index(notices->list, i);
        if (kept->id == id) {
            *index = i;
            return true;
        }
    }
    return false;
}

xd_notice *xd_notices_add(xd_notices *notices, xd_notice *notice, uint32_t replaces_id,
                          bool *replaced) {
    guint index = 0;
    *replaced = false;
    if (replaces_id != 0 && index_of(notices, replaces_id, &index)) {
        xd_notice *old = g_ptr_array_index(notices->list, index);
        /* Only its own program's: no program closes another's. */
        if (old->sender != nullptr && g_strcmp0(old->sender, notice->sender) == 0 && !old->closed) {
            notice->id = old->id;
            g_ptr_array_remove_index(notices->list, index);
            g_ptr_array_add(notices->list, notice);
            *replaced = true;
            return notice;
        }
    }
    notice->id = next_id(notices);
    g_ptr_array_add(notices->list, notice);
    return notice;
}

xd_notice *xd_notices_find(const xd_notices *notices, uint32_t id) {
    guint index = 0;
    return index_of(notices, id, &index) ? g_ptr_array_index(notices->list, index) : nullptr;
}

bool xd_notices_remove(xd_notices *notices, uint32_t id) {
    guint index = 0;
    if (!index_of(notices, id, &index))
        return false;
    g_ptr_array_remove_index(notices->list, index);
    return true;
}

size_t xd_notices_unread(const xd_notices *notices) {
    size_t count = 0;
    for (guint i = 0; i < notices->list->len; i++) {
        const xd_notice *kept = g_ptr_array_index(notices->list, i);
        count += kept->unread ? 1 : 0;
    }
    return count;
}

GArray *xd_notices_trim(xd_notices *notices, size_t max, uint32_t keep) {
    GArray *gone = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    for (guint i = 0; notices->list->len > max && i < notices->list->len;) {
        const xd_notice *oldest = g_ptr_array_index(notices->list, i);
        if (oldest->id == keep) {
            i++;
            continue;
        }
        if (!oldest->closed)
            g_array_append_val(gone, oldest->id);
        g_ptr_array_remove_index(notices->list, i);
    }
    return gone;
}

static constexpr char GROUP_PREFIX[] = "Notification ";

static void set_optional(GKeyFile *key_file, const char *group, const char *key,
                         const char *value) {
    if (value != nullptr && value[0] != '\0')
        g_key_file_set_string(key_file, group, key, value);
}

char *xd_notices_save_data(const xd_notices *notices) {
    GKeyFile *key_file = g_key_file_new();
    int number = 0;
    for (guint i = 0; i < notices->list->len; i++) {
        const xd_notice *kept = g_ptr_array_index(notices->list, i);
        if (kept->transient)
            continue;
        char *group = g_strdup_printf("%s%d", GROUP_PREFIX, number++);
        set_optional(key_file, group, "AppName", kept->app_name);
        set_optional(key_file, group, "DesktopEntry", kept->desktop_entry);
        set_optional(key_file, group, "Icon", kept->icon);
        set_optional(key_file, group, "Summary", kept->summary);
        set_optional(key_file, group, "Body", kept->body);
        g_key_file_set_int64(key_file, group, "Time", kept->time);
        g_key_file_set_boolean(key_file, group, "Unread", kept->unread);
        g_key_file_set_integer(key_file, group, "Urgency", kept->urgency);
        g_free(group);
    }
    char *data = g_key_file_to_data(key_file, nullptr, nullptr);
    g_key_file_unref(key_file);
    return data;
}

static char *get_optional(GKeyFile *key_file, const char *group, const char *key, glong max) {
    char *value = g_key_file_get_string(key_file, group, key, nullptr);
    if (value == nullptr)
        return nullptr;
    char *clean = clean_text(value, max);
    g_free(value);
    return clean;
}

void xd_notices_load_data(xd_notices *notices, const char *data) {
    GKeyFile *key_file = g_key_file_new();
    if (data == nullptr || !g_key_file_load_from_data(key_file, data, (gsize)-1, 0, nullptr)) {
        g_key_file_unref(key_file);
        return;
    }
    gsize count = 0;
    char **groups = g_key_file_get_groups(key_file, &count);
    for (gsize i = 0; i < count; i++) {
        if (!g_str_has_prefix(groups[i], GROUP_PREFIX))
            continue;
        xd_notice *notice = XD_NEW(xd_notice);
        notice->app_name = get_optional(key_file, groups[i], "AppName", APP_NAME_MAX);
        char *entry = get_optional(key_file, groups[i], "DesktopEntry", 200);
        notice->desktop_entry = desktop_entry_of(entry);
        g_free(entry);
        char *icon = get_optional(key_file, groups[i], "Icon", 4096);
        notice->icon = icon_of(icon);
        g_free(icon);
        notice->summary = get_optional(key_file, groups[i], "Summary", SUMMARY_MAX);
        notice->body = get_optional(key_file, groups[i], "Body", BODY_MAX);
        if (notice->app_name == nullptr)
            notice->app_name = g_strdup("");
        if (notice->summary == nullptr)
            notice->summary = g_strdup("");
        if (notice->body == nullptr)
            notice->body = g_strdup("");
        notice->actions = g_new0(char *, 1);
        notice->time = g_key_file_get_int64(key_file, groups[i], "Time", nullptr);
        notice->unread = g_key_file_get_boolean(key_file, groups[i], "Unread", nullptr);
        int urgency = g_key_file_get_integer(key_file, groups[i], "Urgency", nullptr);
        notice->urgency = urgency == XD_NOTICE_LOW        ? XD_NOTICE_LOW
                          : urgency == XD_NOTICE_CRITICAL ? XD_NOTICE_CRITICAL
                                                          : XD_NOTICE_NORMAL;
        notice->expire_ms = -1;
        notice->closed = true;
        notice->id = next_id(notices);
        g_ptr_array_add(notices->list, notice);
    }
    g_strfreev(groups);
    g_key_file_unref(key_file);
}
