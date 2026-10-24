/* SPDX-License-Identifier: MIT
 * Disk Cleanup's Unused Flatpak runtimes. See flatpakclean.h.
 */
#include "flatpakclean.h"

#include <flatpak.h>

/* One installation's unused refs, as the category keeps them. */
typedef struct unused {
    FlatpakInstallation *installation;
    GPtrArray *refs; /* FlatpakInstalledRef */
} unused;

static void unused_free(gpointer data) {
    unused *found = data;
    g_object_unref(found->installation);
    g_ptr_array_unref(found->refs);
    g_free(found);
}

XdCleanupCategory *xd_flatpak_unused_scan(const XdCleanupPlaces *cleanup_places,
                                          GCancellable *cancel) {
    GPtrArray *found = g_ptr_array_new_with_free_func(unused_free);
    guint64 size = 0;
    char *view = nullptr;
    for (int system = 0; system < 2; system++) {
        FlatpakInstallation *installation = system
                                                ? flatpak_installation_new_system(cancel, nullptr)
                                                : flatpak_installation_new_user(cancel, nullptr);
        if (installation == nullptr)
            continue;
        GFile *dir = flatpak_installation_get_path(installation);
        char *path = dir != nullptr ? g_file_get_path(dir) : nullptr;
        GPtrArray *refs = path != nullptr && xd_cleanup_places_holds(cleanup_places, path)
                              ? flatpak_installation_list_unused_refs(installation, nullptr,
                                                                      cancel, nullptr)
                              : nullptr;
        if (refs != nullptr && refs->len > 0) {
            for (guint i = 0; i < refs->len; i++)
                size += flatpak_installed_ref_get_installed_size(g_ptr_array_index(refs, i));
            if (view == nullptr)
                view = g_build_filename(path, "runtime", nullptr);
            unused *one = g_new0(unused, 1);
            one->installation = installation;
            one->refs = refs;
            g_ptr_array_add(found, one);
        } else {
            if (refs != nullptr)
                g_ptr_array_unref(refs);
            g_object_unref(installation);
        }
        g_free(path);
    }
    if (found->len == 0) {
        g_ptr_array_unref(found);
        g_free(view);
        return nullptr;
    }
    XdCleanupCategory *cleanup_category = xd_cleanup_category_new(XD_CLEANUP_FLATPAK);
    cleanup_category->size = size;
    cleanup_category->view = view;
    cleanup_category->extra = found;
    cleanup_category->extra_free = (GDestroyNotify)g_ptr_array_unref;
    return cleanup_category;
}

bool xd_flatpak_unused_remove(const XdCleanupCategory *cleanup_category, GCancellable *cancel,
                              GError **err) {
    GPtrArray *found = cleanup_category->extra;
    bool ok = true;
    for (guint i = 0; found != nullptr && i < found->len; i++) {
        unused *one = g_ptr_array_index(found, i);
        GError *error = nullptr;
        FlatpakTransaction *transaction =
            flatpak_transaction_new_for_installation(one->installation, cancel, &error);
        bool done = transaction != nullptr;
        for (guint j = 0; done && j < one->refs->len; j++) {
            char *ref = flatpak_ref_format_ref(FLATPAK_REF(g_ptr_array_index(one->refs, j)));
            done = flatpak_transaction_add_uninstall(transaction, ref, &error);
            g_free(ref);
        }
        done = done && flatpak_transaction_run(transaction, cancel, &error);
        if (!done && ok) {
            ok = false;
            g_propagate_error(err, error);
            error = nullptr;
        }
        g_clear_error(&error);
        g_clear_object(&transaction);
    }
    return ok;
}
