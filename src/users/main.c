/* SPDX-License-Identifier: MIT
 * xd-users: Windows XP's User Accounts, through AccountsService
 * (org.freedesktop.Accounts). As in XP it's a set of task pages: pick a
 * task or an account, then what to change about it.
 *
 *   your own account   name and picture (AccountsService lets you change
 *                      your own data), password (Windows Security's
 *                      Change Password, which runs passwd(1) so the
 *                      system's password rules apply)
 *   other accounts     name, picture, password, account type, delete
 *   new accounts       name, then account type
 *
 * Changing other accounts is an administrator's job: AccountsService asks
 * polkit, so xd-polkit asks for an administrator's password. The picture
 * is also copied to ~/.face, which the Start menu shows.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "users.h"

#define ACCOUNTS "org.freedesktop.Accounts"
#define ACCOUNTS_PATH "/org/freedesktop/Accounts"
#define USER_IFACE "org.freedesktop.Accounts.User"
#define TITLE "User Accounts"
/* Pictures are stored at XP's size, doubled for HiDPI. */
static constexpr int PICTURE_SIZE = 96;

typedef enum page_kind {
    PAGE_HOME,
    PAGE_ACCOUNT,
    PAGE_NAME,
    PAGE_PICTURE,
    PAGE_TYPE,
    PAGE_PASSWORD,
    PAGE_DELETE,
    PAGE_DELETE_CONFIRM,
    PAGE_CREATE_NAME,
    PAGE_CREATE_TYPE,
} page_kind;

typedef struct nav {
    page_kind kind;
    char *user; /* the account's object path, or nullptr */
} nav;

typedef struct ua {
    GtkWidget *window, *body, *back, *pane, *main;
    guint calls; /* AccountsService calls under way */
    GDBusProxy *manager;
    GPtrArray *accounts; /* GDBusProxy, by display name */
    GArray *history;     /* nav, for Back */
    nav cur;
    char *my_path;
    guint rebuild_idle;
    /* page state */
    GtkWidget *entry[4], *admin, *flow, *ok;
    char *picture; /* the picture chosen on the Picture page */
    bool keep_files;
    bool name_edited; /* the user name was typed, not suggested */
    bool suggesting;  /* we're filling it in */
    char *new_full, *new_name; /* the account being created */
} ua;

/* ---- AccountsService --------------------------------------------------------- */

static char *str_prop(GDBusProxy *p, const char *name) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    char *s = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                  ? g_variant_dup_string(v, nullptr)
                  : g_strdup("");
    if (v != nullptr)
        g_variant_unref(v);
    return s;
}

static int int_prop(GDBusProxy *p, const char *name) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    int i = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_INT32) ? g_variant_get_int32(v)
                                                                         : 0;
    if (v != nullptr)
        g_variant_unref(v);
    return i;
}

static bool bool_prop(GDBusProxy *p, const char *name) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    bool b = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) &&
             g_variant_get_boolean(v);
    if (v != nullptr)
        g_variant_unref(v);
    return b;
}

static guint64 uid_prop(GDBusProxy *p) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, "Uid");
    guint64 u = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64)
                    ? g_variant_get_uint64(v)
                    : G_MAXUINT64;
    if (v != nullptr)
        g_variant_unref(v);
    return u;
}

/* The name Windows shows: the full name, else the user name. */
static char *display_name(GDBusProxy *p) {
    char *real = str_prop(p, "RealName");
    if (real[0] != '\0')
        return real;
    g_free(real);
    return str_prop(p, "UserName");
}

static bool is_admin(GDBusProxy *p) { return int_prop(p, "AccountType") == XD_ACCOUNT_ADMIN; }

static GDBusProxy *find_account(ua *u, const char *path) {
    for (guint i = 0; path != nullptr && i < u->accounts->len; i++) {
        GDBusProxy *p = g_ptr_array_index(u->accounts, i);
        if (strcmp(g_dbus_proxy_get_object_path(p), path) == 0)
            return p;
    }
    return nullptr;
}

static bool is_me(ua *u, GDBusProxy *p) {
    return u->my_path != nullptr && strcmp(g_dbus_proxy_get_object_path(p), u->my_path) == 0;
}

static void error_box(ua *u, const char *what, GError *error) {
    g_dbus_error_strip_remote_error(error);
    char *s = g_strdup_printf("%s\n\n%s", what, error->message);
    xd_message_box(GTK_WINDOW(u->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, s);
    g_free(s);
}

/* What to do with a call's reply once it succeeded. */
typedef void (*reply_fn)(struct ua *u, GVariant *reply);

typedef struct pending_call {
    ua *u;
    char *what; /* the error box's text if it fails */
    reply_fn done;
} pending_call;

static void call_done(GObject *src, GAsyncResult *res, gpointer data) {
    pending_call *c = data;
    ua *u = c->u;
    GError *error = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error);
    if (--u->calls == 0)
        gtk_widget_set_sensitive(u->body, TRUE);
    if (r == nullptr) {
        error_box(u, c->what, error);
        g_error_free(error);
    } else {
        if (c->done != nullptr)
            c->done(u, r);
        g_variant_unref(r);
    }
    g_free(c->what);
    g_free(c);
}

/* Calls AccountsService without waiting: polkit's password prompt may be
 * up for a while, and the window must keep drawing and answering
 * meanwhile. It's greyed until the reply, so the page stays as it was;
 * then `done` runs, or an error box says `what` failed. */
static void call(ua *u, GDBusProxy *p, const char *method, GVariant *args, const char *what,
                 reply_fn done) {
    pending_call *c = g_new0(pending_call, 1);
    c->u = u;
    c->what = g_strdup(what);
    c->done = done;
    if (u->calls++ == 0)
        gtk_widget_set_sensitive(u->body, FALSE);
    g_dbus_proxy_call(p, method, args, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                      G_MAXINT, nullptr, call_done, c);
}

static void build_page(ua *u);

static gboolean rebuild_now(gpointer data) {
    ua *u = data;
    u->rebuild_idle = 0;
    /* The account may be gone (deleted elsewhere). */
    if (u->cur.user != nullptr && find_account(u, u->cur.user) == nullptr) {
        g_clear_pointer(&u->cur.user, g_free);
        u->cur.kind = PAGE_HOME;
    }
    build_page(u);
    return G_SOURCE_REMOVE;
}

/* Pages that only show accounts are rebuilt when an account changes;
 * forms aren't, so nothing typed is lost. */
static void account_changed(ua *u) {
    bool showing = u->cur.kind == PAGE_HOME || u->cur.kind == PAGE_ACCOUNT;
    if (showing && u->rebuild_idle == 0)
        u->rebuild_idle = g_idle_add(rebuild_now, u);
}

static void on_user_signal(GDBusProxy *p, [[maybe_unused]] const char *sender,
                           const char *signal, [[maybe_unused]] GVariant *params, gpointer data);

static GDBusProxy *open_account(ua *u, const char *path) {
    GDBusProxy *p = g_dbus_proxy_new_sync(g_dbus_proxy_get_connection(u->manager),
                                          G_DBUS_PROXY_FLAGS_NONE, nullptr, ACCOUNTS, path,
                                          USER_IFACE, nullptr, nullptr);
    if (p != nullptr) {
        g_signal_connect(p, "g-signal", G_CALLBACK(on_user_signal), u);
        g_signal_connect_swapped(p, "g-properties-changed", G_CALLBACK(account_changed), u);
    }
    return p;
}

static int compare_accounts(gconstpointer a, gconstpointer b) {
    GDBusProxy *x = *(GDBusProxy *const *)a, *y = *(GDBusProxy *const *)b;
    char *nx = display_name(x), *ny = display_name(y);
    int r = g_utf8_collate(nx, ny);
    g_free(nx);
    g_free(ny);
    return r;
}

static void load_accounts(ua *u) {
    g_ptr_array_set_size(u->accounts, 0);
    g_clear_pointer(&u->my_path, g_free);
    if (u->manager == nullptr)
        return;
    GVariant *r = g_dbus_proxy_call_sync(u->manager, "ListCachedUsers", nullptr,
                                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    if (r == nullptr)
        return;
    GVariantIter *it = nullptr;
    const char *path = nullptr;
    g_variant_get(r, "(ao)", &it);
    while (g_variant_iter_loop(it, "&o", &path)) {
        GDBusProxy *p = open_account(u, path);
        if (p == nullptr)
            continue;
        if (bool_prop(p, "SystemAccount")) {
            g_object_unref(p);
            continue;
        }
        if (uid_prop(p) == (guint64)getuid())
            u->my_path = g_strdup(path);
        g_ptr_array_add(u->accounts, p);
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    g_ptr_array_sort(u->accounts, compare_accounts);
}

/* Older AccountsService only says "Changed", without the properties:
 * read the account again. */
static void on_user_signal(GDBusProxy *p, [[maybe_unused]] const char *sender,
                           const char *signal, [[maybe_unused]] GVariant *params, gpointer data) {
    ua *u = data;
    if (strcmp(signal, "Changed") != 0)
        return;
    for (guint i = 0; i < u->accounts->len; i++) {
        if (g_ptr_array_index(u->accounts, i) != p)
            continue;
        GDBusProxy *fresh = open_account(u, g_dbus_proxy_get_object_path(p));
        if (fresh != nullptr) {
            /* The old proxy is still emitting; the array's reference is
             * dropped once it's done. */
            g_signal_handlers_disconnect_by_data(p, u);
            g_ptr_array_index(u->accounts, i) = fresh;
            g_idle_add_once((GSourceOnceFunc)g_object_unref, p);
        }
        break;
    }
    g_ptr_array_sort(u->accounts, compare_accounts);
    account_changed(u);
}

static void on_manager_signal([[maybe_unused]] GDBusProxy *p, [[maybe_unused]] const char *sender,
                              const char *signal, [[maybe_unused]] GVariant *params,
                              gpointer data) {
    ua *u = data;
    if (strcmp(signal, "UserAdded") == 0 || strcmp(signal, "UserDeleted") == 0) {
        load_accounts(u);
        account_changed(u);
    }
}

/* ---- navigation ------------------------------------------------------------------ */

static void go(ua *u, page_kind kind, const char *user) {
    g_array_append_val(u->history, u->cur);
    u->cur = (nav){kind, g_strdup(user)};
    build_page(u);
}

static void go_back(ua *u) {
    if (u->history->len == 0)
        return;
    g_free(u->cur.user);
    u->cur = g_array_index(u->history, nav, u->history->len - 1);
    g_array_set_size(u->history, u->history->len - 1);
    build_page(u);
}

static void clear_history(ua *u) {
    for (guint i = 0; i < u->history->len; i++)
        g_free(g_array_index(u->history, nav, i).user);
    g_array_set_size(u->history, 0);
}

/* Home, forgetting the way there (after an account is created or
 * deleted, Back has nowhere sensible to go). */
static void go_home(ua *u) {
    clear_history(u);
    g_free(u->cur.user);
    u->cur = (nav){PAGE_HOME, nullptr};
    build_page(u);
}

/* Back to the account's page after a change, as XP does. */
static void done(ua *u) {
    while (u->history->len > 0 && u->cur.kind != PAGE_ACCOUNT && u->cur.kind != PAGE_HOME) {
        g_free(u->cur.user);
        u->cur = g_array_index(u->history, nav, u->history->len - 1);
        g_array_set_size(u->history, u->history->len - 1);
    }
    build_page(u);
}

/* ---- widgets ---------------------------------------------------------------------- */

static const char css[] =
    ".xd-ua-pane { background-image: linear-gradient(to bottom, #7ba2e7, #6375d6); }\n"
    ".xd-ua-box { background-color: #d6dff7; border-radius: 3px 3px 0 0; }\n"
    ".xd-ua-box-title { color: #215dc6; font-weight: bold; padding: 4px 8px;"
    " background-image: linear-gradient(to right, #ffffff, #c6d3f7); border-radius: 3px 3px 0 0; }\n"
    ".xd-ua-main { background-color: #ffffff; }\n"
    ".xd-ua-title { color: #4b6fb5; font-size: 18pt; }\n"
    ".xd-ua-heading { color: #4b6fb5; font-size: 13pt; }\n"
    "button.xd-ua-link { background: none; border: none; box-shadow: none; padding: 1px 2px; }\n"
    "button.xd-ua-link label { color: #0033cc; }\n"
    "button.xd-ua-link:hover label { color: #3366ff; text-decoration: underline; }\n"
    ".xd-ua-pane button.xd-ua-link label { color: #215dc6; }\n"
    "button.xd-ua-tile { background: none; border: 1px solid transparent; box-shadow: none; }\n"
    "button.xd-ua-tile:hover { border-color: #7ba2e7; background-color: #eef3fd; }\n"
    ".xd-ua-picture { border: 2px solid #ffffff; box-shadow: 0 0 0 1px #8ea6d6; }\n"
    ".xd-ua-dim { color: #555555; }\n";

static GtkWidget *label(const char *text, const char *class) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    if (class != nullptr)
        gtk_style_context_add_class(gtk_widget_get_style_context(l), class);
    return l;
}

static void add_class(GtkWidget *w, const char *class) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), class);
}

/* A task: a green arrow and a blue link, as on XP's task pages. */
static GtkWidget *task(const char *text, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new();
    add_class(b, "xd-ua-link");
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new_from_icon_name("go-next", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(b), row);
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    g_signal_connect_swapped(b, "clicked", cb, data);
    return b;
}

/* The account picture: AccountsService's, else a generic one. */
static GtkWidget *picture(GDBusProxy *p, int size) {
    char *file = p != nullptr ? str_prop(p, "IconFile") : g_strdup("");
    GdkPixbuf *pb = file[0] != '\0' ? gdk_pixbuf_new_from_file_at_scale(file, size, size, FALSE,
                                                                        nullptr)
                                    : nullptr;
    g_free(file);
    GtkWidget *img;
    if (pb != nullptr) {
        img = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
    } else {
        img = gtk_image_new_from_icon_name("avatar-default", GTK_ICON_SIZE_DIALOG);
        gtk_image_set_pixel_size(GTK_IMAGE(img), size);
    }
    add_class(img, "xd-ua-picture");
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    return img;
}

static void unwrap(GtkWidget *w, [[maybe_unused]] gpointer data) {
    gtk_label_set_line_wrap(GTK_LABEL(w), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(w), 24);
}

/* Picture, name, account type, password: XP's account tile. */
static GtkWidget *tile_content(GDBusProxy *p) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(row), picture(p, 48), FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    char *name = display_name(p);
    char *markup = g_markup_printf_escaped("<b>%s</b>", name);
    GtkWidget *n = label("", nullptr);
    gtk_label_set_markup(GTK_LABEL(n), markup);
    gtk_box_pack_start(GTK_BOX(col), n, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(col),
                       label(is_admin(p) ? "Computer administrator" : "Limited account",
                             "xd-ua-dim"),
                       FALSE, FALSE, 0);
    if (bool_prop(p, "Locked"))
        gtk_box_pack_start(GTK_BOX(col), label("Account disabled", "xd-ua-dim"), FALSE, FALSE, 0);
    else if (int_prop(p, "PasswordMode") == 0)
        gtk_box_pack_start(GTK_BOX(col), label("Password protected", "xd-ua-dim"), FALSE, FALSE,
                           0);
    /* One line each, so tiles stay narrow enough for two a row. */
    gtk_container_foreach(GTK_CONTAINER(col), unwrap, nullptr);
    gtk_box_pack_start(GTK_BOX(row), col, FALSE, FALSE, 0);
    g_free(markup);
    g_free(name);
    return row;
}

static void on_tile(GtkButton *b, gpointer data) {
    go(data, PAGE_ACCOUNT, g_object_get_data(G_OBJECT(b), "path"));
}

static GtkWidget *tile(ua *u, GDBusProxy *p) {
    GtkWidget *b = gtk_button_new();
    add_class(b, "xd-ua-tile");
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(b), tile_content(p));
    gtk_widget_set_size_request(b, 230, -1);
    g_object_set_data_full(G_OBJECT(b), "path", g_strdup(g_dbus_proxy_get_object_path(p)), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_tile), u);
    return b;
}

/* OK-style buttons at the bottom right of a form. */
static GtkWidget *button(const char *text, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_size_request(b, 90, -1);
    g_signal_connect_swapped(b, "clicked", cb, data);
    return b;
}

/* Two or three of them in a row (`third` may be nullptr). */
static GtkWidget *button_row(GtkWidget *first, GtkWidget *second, GtkWidget *third) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *buttons[] = {first, second, third};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
        if (buttons[i] != nullptr)
            gtk_box_pack_start(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    }
    return row;
}

static GtkWidget *entry(ua *u, int i, const char *text, bool password) {
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), text != nullptr ? text : "");
    gtk_entry_set_visibility(GTK_ENTRY(e), !password);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(e), 32);
    gtk_widget_set_halign(e, GTK_ALIGN_START);
    u->entry[i] = e;
    return e;
}

static const char *entry_text(ua *u, int i) { return gtk_entry_get_text(GTK_ENTRY(u->entry[i])); }

/* A blue box in the task pane: a title and links. */
static GtkWidget *pane_box(const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_class(box, "xd-ua-box");
    GtkWidget *t = label(title, "xd-ua-box-title");
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    return box;
}

static void pane_add(GtkWidget *box, GtkWidget *link) {
    gtk_widget_set_margin_start(link, 6);
    gtk_box_pack_start(GTK_BOX(box), link, FALSE, FALSE, 0);
}

/* ---- actions --------------------------------------------------------------------- */

static GDBusProxy *cur_account(ua *u) { return find_account(u, u->cur.user); }

static void run_change_password(ua *u) {
    /* Our own Change Password (xd-security), next to us or on PATH. */
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : g_strdup(".");
    char *local = g_build_filename(dir, "xd-security", nullptr);
    const char *argv[] = {g_file_test(local, G_FILE_TEST_IS_EXECUTABLE) ? local : "xd-security",
                          "--change-password", nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        error_box(u, "Change Password could not be started.", error);
        g_error_free(error);
    }
    g_free(local);
    g_free(dir);
    g_free(self);
}

static void changed(ua *u, [[maybe_unused]] GVariant *reply) { done(u); }

static void on_change_name(ua *u) {
    GDBusProxy *p = cur_account(u);
    char *name = g_strstrip(g_strdup(entry_text(u, 0)));
    if (p != nullptr)
        call(u, p, "SetRealName", g_variant_new("(s)", name), "The name could not be changed.",
             changed);
    g_free(name);
}

/* The picture for `p`, made from `source`. Our own goes to ~/.face, which
 * the Start menu (and other desktops' greeters) read; another account's
 * stays in our cache. AccountsService copies it from there, but keeps the
 * name it was given when it can't (as python-dbusmock does), so the file
 * stays. */
static void set_picture(ua *u, GDBusProxy *p, const char *source) {
    char *path;
    if (is_me(u, p)) {
        path = g_build_filename(g_get_home_dir(), ".face", nullptr);
    } else {
        char *dir = g_build_filename(g_get_user_cache_dir(), "xeondesktop", nullptr);
        g_mkdir_with_parents(dir, 0700);
        char *name = g_strdup_printf("account-picture-%" G_GUINT64_FORMAT ".png", uid_prop(p));
        path = g_build_filename(dir, name, nullptr);
        g_free(name);
        g_free(dir);
    }
    char *tmp = g_strconcat(path, ".new", nullptr);
    GError *error = nullptr;
    bool made = xd_users_make_picture(source, tmp, PICTURE_SIZE, &error);
    if (made && g_rename(tmp, path) != 0) {
        GFileError code = g_file_error_from_errno(errno);
        g_set_error(&error, G_FILE_ERROR, (gint)code, "%s: %s", path,
                    g_strerror(errno));
        made = false;
    }
    if (!made) {
        error_box(u, "This picture can't be used.", error);
        g_error_free(error);
        g_unlink(tmp);
    } else {
        call(u, p, "SetIconFile", g_variant_new("(s)", path), "The picture could not be changed.",
             changed);
    }
    g_free(tmp);
    g_free(path);
}

static void on_change_picture(ua *u) {
    GDBusProxy *p = cur_account(u);
    if (p != nullptr && u->picture != nullptr)
        set_picture(u, p, u->picture);
}

static void on_browse(ua *u) {
    GtkWidget *d = gtk_file_chooser_dialog_new("Open", GTK_WINDOW(u->window),
                                               GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
                                               GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT,
                                               nullptr);
    GtkFileFilter *pictures = gtk_file_filter_new();
    gtk_file_filter_set_name(pictures, "Pictures (*.bmp;*.jpg;*.png;*.webp)");
    gtk_file_filter_add_pixbuf_formats(pictures);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), pictures);
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pics != nullptr)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d), pics);
    char *file = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT
                     ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d))
                     : nullptr;
    gtk_widget_destroy(d);
    GDBusProxy *p = cur_account(u);
    if (file != nullptr && p != nullptr)
        set_picture(u, p, file);
    g_free(file);
}

static int count_admins(ua *u) {
    int n = 0;
    for (guint i = 0; i < u->accounts->len; i++)
        n += is_admin(g_ptr_array_index(u->accounts, i)) ? 1 : 0;
    return n;
}

static void on_change_type(ua *u) {
    GDBusProxy *p = cur_account(u);
    if (p == nullptr)
        return;
    bool admin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->admin));
    if (admin == is_admin(p)) {
        done(u);
        return;
    }
    if (!admin && count_admins(u) <= 1) {
        xd_message_box(GTK_WINDOW(u->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "This is the only computer administrator account. At least one account "
                       "must be an administrator, so it can't be changed to a limited account.");
        return;
    }
    call(u, p, "SetAccountType", g_variant_new("(i)", admin ? XD_ACCOUNT_ADMIN : XD_ACCOUNT_STANDARD),
         "The account type could not be changed.", changed);
}

static void on_set_password(ua *u) {
    GDBusProxy *p = cur_account(u);
    if (p == nullptr)
        return;
    const char *pw = entry_text(u, 0), *again = entry_text(u, 1);
    if (strcmp(pw, again) != 0) {
        xd_message_box(GTK_WINDOW(u->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "The passwords you typed do not match. Type the new password in both "
                       "text boxes.");
        return;
    }
    if (pw[0] == '\0') {
        xd_message_box(GTK_WINDOW(u->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "Type a password.");
        return;
    }
    char *hash = xd_users_hash_password(pw);
    if (hash == nullptr) {
        xd_message_box(GTK_WINDOW(u->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The password could not be encrypted.");
        return;
    }
    call(u, p, "SetPassword", g_variant_new("(ss)", hash, entry_text(u, 2)),
         "The password could not be changed.", changed);
    explicit_bzero(hash, strlen(hash));
    g_free(hash);
}

static void on_delete_files(ua *u) {
    u->keep_files = false;
    go(u, PAGE_DELETE_CONFIRM, u->cur.user);
}

static void on_keep_files(ua *u) {
    u->keep_files = true;
    go(u, PAGE_DELETE_CONFIRM, u->cur.user);
}

static void deleted(ua *u, [[maybe_unused]] GVariant *reply) {
    load_accounts(u);
    go_home(u);
}

static void on_delete(ua *u) {
    GDBusProxy *p = cur_account(u);
    if (p == nullptr || u->manager == nullptr)
        return;
    call(u, u->manager, "DeleteUser", g_variant_new("(xb)", (gint64)uid_prop(p), !u->keep_files),
         "The account could not be deleted.", deleted);
}

static void on_full_name_changed([[maybe_unused]] GtkEditable *e, gpointer data) {
    ua *u = data;
    if (u->name_edited)
        return;
    char *s = xd_users_suggest_name(entry_text(u, 0));
    u->suggesting = true;
    gtk_entry_set_text(GTK_ENTRY(u->entry[1]), s);
    u->suggesting = false;
    g_free(s);
}

static void on_user_name_changed([[maybe_unused]] GtkEditable *e, gpointer data) {
    ua *u = data;
    if (!u->suggesting)
        u->name_edited = true;
}

static void on_create_next(ua *u) {
    char *full = g_strstrip(g_strdup(entry_text(u, 0)));
    const char *name = entry_text(u, 1);
    if (!xd_users_valid_name(name)) {
        xd_message_box(GTK_WINDOW(u->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "The user name must start with a lowercase letter and contain only "
                       "lowercase letters, digits, hyphens and underscores (at most 32).");
        g_free(full);
        return;
    }
    g_free(u->new_full);
    g_free(u->new_name);
    u->new_full = full;
    u->new_name = g_strdup(name);
    go(u, PAGE_CREATE_TYPE, nullptr);
}

static void created(ua *u, GVariant *r) {
    const char *path = nullptr;
    g_variant_get(r, "(&o)", &path);
    load_accounts(u);
    /* On to the new account, where it can get a password and picture. */
    clear_history(u);
    g_free(u->cur.user);
    u->cur = (nav){PAGE_HOME, nullptr};
    if (find_account(u, path) != nullptr)
        go(u, PAGE_ACCOUNT, path);
    else
        build_page(u);
}

static void on_create(ua *u) {
    bool admin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->admin));
    call(u, u->manager, "CreateUser",
         g_variant_new("(ssi)", u->new_name, u->new_full,
                       admin ? XD_ACCOUNT_ADMIN : XD_ACCOUNT_STANDARD),
         "The account could not be created.", created);
}

static void on_go_name(ua *u) { go(u, PAGE_NAME, u->cur.user); }
static void on_go_picture(ua *u) { go(u, PAGE_PICTURE, u->cur.user); }
static void on_go_type(ua *u) { go(u, PAGE_TYPE, u->cur.user); }
static void on_go_delete(ua *u) { go(u, PAGE_DELETE, u->cur.user); }
static void on_go_create(ua *u) {
    g_clear_pointer(&u->new_full, g_free);
    g_clear_pointer(&u->new_name, g_free);
    go(u, PAGE_CREATE_NAME, nullptr);
}
static void on_go_change(ua *u) { go_home(u); }
static void on_go_mine(ua *u) { go(u, PAGE_ACCOUNT, u->my_path); }

static void on_go_password(ua *u) {
    GDBusProxy *p = cur_account(u);
    if (p != nullptr && is_me(u, p))
        run_change_password(u);
    else
        go(u, PAGE_PASSWORD, u->cur.user);
}

static void on_profiles(ua *u) {
    const char *argv[] = {"xd-sysdm", "--page", "3", nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        error_box(u, "System Properties could not be started.", error);
        g_error_free(error);
    }
}

/* ---- pages ------------------------------------------------------------------------- */

static void page_home(ua *u, GtkWidget *box) {
    gtk_box_pack_start(GTK_BOX(box), label("Pick a task...", "xd-ua-title"), FALSE, FALSE, 0);
    GtkWidget *tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(tasks, 20);
    if (u->my_path != nullptr)
        gtk_box_pack_start(GTK_BOX(tasks), task("Change my account", G_CALLBACK(on_go_mine), u),
                           FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tasks),
                       task("Create a new account", G_CALLBACK(on_go_create), u), FALSE, FALSE,
                       0);
    gtk_box_pack_start(GTK_BOX(box), tasks, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), label("or pick an account to change", "xd-ua-title"), FALSE,
                       FALSE, 12);
    /* Two a row, as in XP. */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 20);
    for (guint i = 0; i < u->accounts->len; i++)
        gtk_grid_attach(GTK_GRID(grid), tile(u, g_ptr_array_index(u->accounts, i)), (int)(i % 2),
                        (int)(i / 2), 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    if (u->manager == nullptr)
        gtk_box_pack_start(GTK_BOX(box),
                           label("Accounts can't be shown: AccountsService isn't available.",
                                 nullptr),
                           FALSE, FALSE, 0);
}

static void page_account(ua *u, GtkWidget *box, GDBusProxy *p) {
    bool me = is_me(u, p);
    char *name = display_name(p);
    char *title = me ? g_strdup("What do you want to change about your account?")
                     : g_strdup_printf("What do you want to change about %s's account?", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    GtkWidget *tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(tasks, 20);
    bool has_password = int_prop(p, "PasswordMode") == 0 && !bool_prop(p, "Locked");
    struct {
        const char *mine, *other;
        GCallback cb;
        bool show;
    } tasks_list[] = {
        {"Change my name", "Change the name", G_CALLBACK(on_go_name), true},
        {"Change my password",
         has_password ? "Change the password" : "Create a password", G_CALLBACK(on_go_password),
         true},
        {"Change my picture", "Change the picture", G_CALLBACK(on_go_picture), true},
        {"Change my account type", "Change the account type", G_CALLBACK(on_go_type), true},
        /* XP doesn't let you delete the account you're using. */
        {nullptr, "Delete the account", G_CALLBACK(on_go_delete), !me},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(tasks_list); i++) {
        if (tasks_list[i].show)
            gtk_box_pack_start(GTK_BOX(tasks),
                               task(me ? tasks_list[i].mine : tasks_list[i].other,
                                    tasks_list[i].cb, u),
                               FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), tasks, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), tile_content(p), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    g_free(title);
    g_free(name);
}

static void page_name(ua *u, GtkWidget *box, GDBusProxy *p) {
    char *name = display_name(p);
    char *title = is_me(u, p) ? g_strdup("Provide a new name for your account")
                              : g_strdup_printf("Provide a new name for %s's account", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Type a new name. It appears on the Start menu and the Welcome "
                             "screen.",
                             nullptr),
                       FALSE, FALSE, 0);
    char *real = str_prop(p, "RealName");
    gtk_box_pack_start(GTK_BOX(box), entry(u, 0, real, false), FALSE, FALSE, 0);
    u->ok = button("Change _Name", G_CALLBACK(on_change_name), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("Cancel", G_CALLBACK(go_back), u), nullptr), FALSE,
                     FALSE, 0);
    g_free(real);
    g_free(title);
    g_free(name);
}

static void on_picture_selected(GtkFlowBox *f, gpointer data) {
    ua *u = data;
    GList *sel = gtk_flow_box_get_selected_children(f);
    g_free(u->picture);
    u->picture = sel != nullptr ? g_strdup(g_object_get_data(sel->data, "file")) : nullptr;
    g_list_free(sel);
    gtk_widget_set_sensitive(u->ok, u->picture != nullptr);
}

static bool is_image(const char *name) {
    static const char *const ext[] = {".png", ".jpg", ".jpeg", ".svg", ".bmp", ".webp"};
    char *lower = g_ascii_strdown(name, -1);
    bool yes = false;
    for (size_t i = 0; i < XD_ARRAY_LEN(ext) && !yes; i++)
        yes = g_str_has_suffix(lower, ext[i]);
    g_free(lower);
    return yes;
}

/* Pictures other desktops ship for accounts (GNOME's faces, Plasma's
 * avatars), one level of folders deep. */
static void find_pictures(const char *dir, int depth, GPtrArray *out) {
    GDir *d = g_dir_open(dir, 0, nullptr);
    if (d == nullptr)
        return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (const char *n = g_dir_read_name(d); n != nullptr; n = g_dir_read_name(d))
        g_ptr_array_add(names, g_build_filename(dir, n, nullptr));
    g_dir_close(d);
    g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < names->len && out->len < 64; i++) {
        const char *path = g_ptr_array_index(names, i);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            if (depth > 0)
                find_pictures(path, depth - 1, out);
        } else if (is_image(path)) {
            g_ptr_array_add(out, g_strdup(path));
        }
    }
    g_ptr_array_free(names, TRUE);
}

static void page_picture(ua *u, GtkWidget *box, GDBusProxy *p) {
    char *name = display_name(p);
    char *title = is_me(u, p) ? g_strdup("Pick a new picture for your account")
                              : g_strdup_printf("Pick a new picture for %s's account", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       label("The picture you choose will appear on the Start menu and the "
                             "Welcome screen.",
                             nullptr),
                       FALSE, FALSE, 0);
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    const char *const *dirs = g_get_system_data_dirs();
    static const char *const sub[] = {"xeondesktop/faces", "pixmaps/faces", "plasma/avatars"};
    for (size_t s = 0; s < XD_ARRAY_LEN(sub); s++) {
        for (const char *const *d = dirs; *d != nullptr; d++) {
            char *dir = g_build_filename(*d, sub[s], nullptr);
            find_pictures(dir, 1, files);
            g_free(dir);
        }
    }
    u->flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(u->flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(u->flow), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(u->flow), 8);
    gtk_widget_set_valign(u->flow, GTK_ALIGN_START);
    for (guint i = 0; i < files->len; i++) {
        const char *f = g_ptr_array_index(files, i);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(f, 56, 56, TRUE, nullptr);
        if (pb == nullptr)
            continue;
        GtkWidget *img = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
        char *base = g_path_get_basename(f);
        gtk_widget_set_tooltip_text(img, base);
        g_free(base);
        gtk_container_add(GTK_CONTAINER(u->flow), img);
        GtkWidget *child = gtk_widget_get_parent(img);
        g_object_set_data_full(G_OBJECT(child), "file", g_strdup(f), g_free);
    }
    if (files->len == 0)
        gtk_box_pack_start(GTK_BOX(box),
                           label("No pictures are installed; browse for one of your own.",
                                 "xd-ua-dim"),
                           FALSE, FALSE, 0);
    g_ptr_array_free(files, TRUE);
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc), 150);
    gtk_container_add(GTK_CONTAINER(sc), u->flow);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), task("Browse for more pictures", G_CALLBACK(on_browse), u),
                       FALSE, FALSE, 0);
    g_clear_pointer(&u->picture, g_free);
    u->ok = button("Change _Picture", G_CALLBACK(on_change_picture), u);
    gtk_widget_set_sensitive(u->ok, FALSE);
    g_signal_connect(u->flow, "selected-children-changed", G_CALLBACK(on_picture_selected), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("Cancel", G_CALLBACK(go_back), u), nullptr), FALSE,
                     FALSE, 0);
    g_free(title);
    g_free(name);
}

/* The two account types, as XP explains them. */
static void type_choice(ua *u, GtkWidget *box, bool admin) {
    GtkWidget *a = gtk_radio_button_new_with_mnemonic(nullptr, "Computer _administrator");
    GtkWidget *l = gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(a),
                                                                  "_Limited");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(admin ? a : l), TRUE);
    u->admin = a;
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    gtk_box_pack_start(GTK_BOX(row), a, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), l, FALSE, FALSE, 0);
    gtk_widget_set_margin_start(row, 20);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    GtkWidget *text = label(
        "With a computer administrator account, you can:\n"
        "  • Create, change, and delete accounts\n"
        "  • Make system-wide changes\n"
        "  • Install programs and access all files\n\n"
        "With a limited account, you can change your own name, picture and password, and use "
        "the programs installed on this computer; system-wide changes need an administrator's "
        "password.",
        nullptr);
    gtk_widget_set_margin_start(text, 20);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
}

static void page_type(ua *u, GtkWidget *box, GDBusProxy *p) {
    char *name = display_name(p);
    char *title = is_me(u, p) ? g_strdup("Pick a new account type for your account")
                              : g_strdup_printf("Pick a new account type for %s", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    type_choice(u, box, is_admin(p));
    u->ok = button("Change Account _Type", G_CALLBACK(on_change_type), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("Cancel", G_CALLBACK(go_back), u), nullptr), FALSE,
                     FALSE, 0);
    g_free(title);
    g_free(name);
}

static GtkWidget *form_row(GtkWidget *box, const char *text, GtkWidget *field) {
    GtkWidget *l = label(text, nullptr);
    gtk_box_pack_start(GTK_BOX(box), l, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), field, FALSE, FALSE, 0);
    return field;
}

static void page_password(ua *u, GtkWidget *box, GDBusProxy *p) {
    char *name = display_name(p);
    bool has = int_prop(p, "PasswordMode") == 0 && !bool_prop(p, "Locked");
    char *title = g_strdup_printf(has ? "Change %s's password" : "Create a password for %s's "
                                                                 "account",
                                  name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    char *warn = g_strdup_printf(
        "If you do this, %s will lose access to the keyring with saved passwords for web sites "
        "and network resources, which is unlocked with the old password.",
        name);
    if (has)
        gtk_box_pack_start(GTK_BOX(box), label(warn, nullptr), FALSE, FALSE, 0);
    form_row(box, "Type a new password:", entry(u, 0, "", true));
    form_row(box, "Type the new password again to confirm:", entry(u, 1, "", true));
    form_row(box,
             "Type a word or phrase to use as a password hint (everyone who uses this computer "
             "can see it):",
             entry(u, 2, "", false));
    u->ok = button(has ? "Change _Password" : "Create _Password", G_CALLBACK(on_set_password), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("Cancel", G_CALLBACK(go_back), u), nullptr), FALSE,
                     FALSE, 0);
    g_free(warn);
    g_free(title);
    g_free(name);
}

static void page_delete(ua *u, GtkWidget *box, GDBusProxy *p) {
    char *name = display_name(p);
    char *title = g_strdup_printf("Do you want to keep %s's files?", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    char *home = str_prop(p, "HomeDirectory");
    char *text = g_strdup_printf(
        "Before you delete %s's account, you can keep the account's home folder (%s), with "
        "its desktop, documents, pictures and settings, for an administrator to look "
        "through. Or you can delete them along with the account.",
        name, home);
    gtk_box_pack_start(GTK_BOX(box), label(text, nullptr), FALSE, FALSE, 0);
    u->ok = button("_Keep Files", G_CALLBACK(on_keep_files), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("_Delete Files", G_CALLBACK(on_delete_files), u),
                                button("Cancel", G_CALLBACK(go_back), u)),
                     FALSE, FALSE, 0);
    g_free(text);
    g_free(home);
    g_free(title);
    g_free(name);
}

static void page_delete_confirm(ua *u, GtkWidget *box, GDBusProxy *p) {
    char *name = display_name(p);
    char *title = g_strdup_printf("Are you sure you want to delete %s's account?", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    char *text = g_strdup_printf(u->keep_files
                                     ? "%s's home folder will be kept, and the account deleted."
                                     : "All of %s's files, and the account, will be deleted.",
                                 name);
    gtk_box_pack_start(GTK_BOX(box), label(text, nullptr), FALSE, FALSE, 0);
    u->ok = button("_Delete Account", G_CALLBACK(on_delete), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("Cancel", G_CALLBACK(go_back), u), nullptr), FALSE,
                     FALSE, 0);
    g_free(text);
    g_free(title);
    g_free(name);
}

static void page_create_name(ua *u, GtkWidget *box) {
    gtk_box_pack_start(GTK_BOX(box), label("Name the new account", "xd-ua-title"), FALSE, FALSE,
                       0);
    form_row(box, "Type a name for the new account:", entry(u, 0, u->new_full, false));
    gtk_box_pack_start(GTK_BOX(box),
                       label("This name will appear on the Welcome screen and on the Start "
                             "menu.",
                             "xd-ua-dim"),
                       FALSE, FALSE, 0);
    form_row(box, "User name, for logging on:", entry(u, 1, u->new_name, false));
    u->name_edited = u->new_name != nullptr;
    g_signal_connect(u->entry[0], "changed", G_CALLBACK(on_full_name_changed), u);
    g_signal_connect(u->entry[1], "changed", G_CALLBACK(on_user_name_changed), u);
    u->ok = button("_Next >", G_CALLBACK(on_create_next), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(u->ok, button("Cancel", G_CALLBACK(go_home), u), nullptr), FALSE,
                     FALSE, 0);
}

static void page_create_type(ua *u, GtkWidget *box) {
    gtk_box_pack_start(GTK_BOX(box), label("Pick an account type", "xd-ua-title"), FALSE, FALSE,
                       0);
    type_choice(u, box, false);
    u->ok = button("_Create Account", G_CALLBACK(on_create), u);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(button("< _Back", G_CALLBACK(go_back), u), u->ok,
                                button("Cancel", G_CALLBACK(go_home), u)),
                     FALSE, FALSE, 0);
}

static void build_pane(ua *u) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(u->pane));
    for (GList *k = kids; k != nullptr; k = k->next)
        gtk_widget_destroy(k->data);
    g_list_free(kids);
    GtkWidget *related = pane_box("Related Tasks");
    if (u->cur.kind != PAGE_HOME)
        pane_add(related, task("Change another account", G_CALLBACK(on_go_change), u));
    pane_add(related, task("Create a new account", G_CALLBACK(on_go_create), u));
    gtk_box_pack_start(GTK_BOX(u->pane), related, FALSE, FALSE, 0);
    GtkWidget *see = pane_box("See Also");
    pane_add(see, task("User profiles", G_CALLBACK(on_profiles), u));
    gtk_box_pack_start(GTK_BOX(u->pane), see, FALSE, FALSE, 0);
    gtk_widget_show_all(u->pane);
}

static void build_page(ua *u) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(u->main));
    for (GList *k = kids; k != nullptr; k = k->next)
        gtk_widget_destroy(k->data);
    g_list_free(kids);
    for (size_t i = 0; i < XD_ARRAY_LEN(u->entry); i++)
        u->entry[i] = nullptr;
    u->admin = u->flow = u->ok = nullptr;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    GDBusProxy *p = cur_account(u);
    if (u->cur.kind != PAGE_HOME && u->cur.kind < PAGE_CREATE_NAME && p == nullptr)
        u->cur.kind = PAGE_HOME;
    switch (u->cur.kind) {
    case PAGE_HOME:
        page_home(u, box);
        break;
    case PAGE_ACCOUNT:
        page_account(u, box, p);
        break;
    case PAGE_NAME:
        page_name(u, box, p);
        break;
    case PAGE_PICTURE:
        page_picture(u, box, p);
        break;
    case PAGE_TYPE:
        page_type(u, box, p);
        break;
    case PAGE_PASSWORD:
        page_password(u, box, p);
        break;
    case PAGE_DELETE:
        page_delete(u, box, p);
        break;
    case PAGE_DELETE_CONFIRM:
        page_delete_confirm(u, box, p);
        break;
    case PAGE_CREATE_NAME:
        page_create_name(u, box);
        break;
    case PAGE_CREATE_TYPE:
        page_create_type(u, box);
        break;
    }
    gtk_box_pack_start(GTK_BOX(u->main), box, TRUE, TRUE, 0);
    build_pane(u);
    gtk_widget_set_sensitive(u->back, u->history->len > 0);
    gtk_widget_show_all(u->main);
    if (u->ok != nullptr) {
        gtk_widget_set_can_default(u->ok, TRUE);
        gtk_widget_grab_default(u->ok);
    }
    if (u->entry[0] != nullptr)
        gtk_widget_grab_focus(u->entry[0]);
}

/* ---- window ------------------------------------------------------------------------ */

static GtkWidget *toolbar_button(const char *icon, const char *text, GCallback cb, ua *u) {
    GtkWidget *b = gtk_button_new_with_mnemonic(text);
    gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_always_show_image(GTK_BUTTON(b), TRUE);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    g_signal_connect_swapped(b, "clicked", cb, u);
    return b;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data) {
    ua *u = data;
    if (u->calls > 0)
        return FALSE; /* the page waits for a change */
    /* Alt+Left and Backspace (outside text boxes) go back, as in Explorer. */
    bool text = GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(w)));
    if ((e->keyval == GDK_KEY_Left && (e->state & GDK_MOD1_MASK)) ||
        (e->keyval == GDK_KEY_BackSpace && !text)) {
        go_back(u);
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.UserAccounts");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static ua u;
    u.accounts = g_ptr_array_new_with_free_func(g_object_unref);
    u.history = g_array_new(FALSE, TRUE, sizeof(nav));
    GError *error = nullptr;
    u.manager = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                                              ACCOUNTS, ACCOUNTS_PATH, ACCOUNTS, nullptr, &error);
    if (u.manager == nullptr) {
        g_printerr("xd-users: %s\n", error->message);
        g_error_free(error);
    } else {
        g_signal_connect(u.manager, "g-signal", G_CALLBACK(on_manager_signal), &u);
    }
    load_accounts(&u);
    if (u.manager != nullptr && u.accounts->len == 0) {
        g_printerr("xd-users: no accounts from %s\n", ACCOUNTS);
        g_clear_object(&u.manager);
    }

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    u.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(u.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(u.window), "system-users");
    gtk_window_set_default_size(GTK_WINDOW(u.window), 720, 520);
    g_signal_connect(u.window, "key-press-event", G_CALLBACK(on_key), &u);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(u.window), outer);
    u.body = outer;
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 2);
    u.back = toolbar_button("go-previous", "_Back", G_CALLBACK(go_back), &u);
    gtk_box_pack_start(GTK_BOX(bar), u.back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), toolbar_button("go-home", "_Home", G_CALLBACK(go_home), &u),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    u.pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(u.pane), 12);
    gtk_widget_set_size_request(u.pane, 200, -1);
    GtkWidget *pane_bg = gtk_event_box_new();
    add_class(pane_bg, "xd-ua-pane");
    gtk_container_add(GTK_CONTAINER(pane_bg), u.pane);
    gtk_box_pack_start(GTK_BOX(body), pane_bg, FALSE, FALSE, 0);
    u.main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *main_bg = gtk_event_box_new();
    add_class(main_bg, "xd-ua-main");
    gtk_container_add(GTK_CONTAINER(main_bg), u.main);
    gtk_box_pack_start(GTK_BOX(body), main_bg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);

    u.cur = (nav){PAGE_HOME, nullptr};
    build_page(&u);
    g_signal_connect(u.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_widget_show_all(u.window);
    gtk_main();
    clear_history(&u);
    g_array_free(u.history, TRUE);
    g_free(u.cur.user);
    g_ptr_array_free(u.accounts, TRUE);
    g_clear_object(&u.manager);
    g_free(u.my_path);
    g_free(u.picture);
    g_free(u.new_full);
    g_free(u.new_name);
    return EXIT_SUCCESS;
}
