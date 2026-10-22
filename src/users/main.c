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
 * `xd-users --advanced` is XP's other User Accounts (control
 * userpasswords2) instead, for automatic logon.
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
#include "accounts.h"
#include "users.h"

#define ACCOUNTS XD_ACCOUNTS_NAME
#define ACCOUNTS_PATH XD_ACCOUNTS_PATH
#define USER_IFACE XD_ACCOUNTS_USER_IFACE
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
    /* XP's advanced User Accounts (control userpasswords2), --advanced */
    bool advanced;
    GtkWidget *must_logon, *users_view, *apply;
    GtkListStore *users;
    bool closing; /* OK was pressed: close once the change is made */
} ua;

/* ---- AccountsService --------------------------------------------------------- */

static char *str_prop(GDBusProxy *proxy, const char *name) {
    GVariant *variant = g_dbus_proxy_get_cached_property(proxy, name);
    char *text = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_STRING)
                     ? g_variant_dup_string(variant, nullptr)
                     : g_strdup("");
    if (variant != nullptr)
        g_variant_unref(variant);
    return text;
}

static int int_prop(GDBusProxy *proxy, const char *name) {
    GVariant *variant = g_dbus_proxy_get_cached_property(proxy, name);
    int i = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_INT32)
                ? g_variant_get_int32(variant)
                : 0;
    if (variant != nullptr)
        g_variant_unref(variant);
    return i;
}

static bool bool_prop(GDBusProxy *proxy, const char *name) {
    GVariant *variant = g_dbus_proxy_get_cached_property(proxy, name);
    bool value = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_BOOLEAN) &&
                 g_variant_get_boolean(variant);
    if (variant != nullptr)
        g_variant_unref(variant);
    return value;
}

static guint64 uid_prop(GDBusProxy *proxy) {
    GVariant *variant = g_dbus_proxy_get_cached_property(proxy, "Uid");
    guint64 uid = variant != nullptr && g_variant_is_of_type(variant, G_VARIANT_TYPE_UINT64)
                      ? g_variant_get_uint64(variant)
                      : G_MAXUINT64;
    if (variant != nullptr)
        g_variant_unref(variant);
    return uid;
}

/* The name Windows shows: the full name, else the user name. */
static char *display_name(GDBusProxy *proxy) {
    char *real = str_prop(proxy, "RealName");
    if (real[0] != '\0')
        return real;
    g_free(real);
    return str_prop(proxy, "UserName");
}

static bool is_admin(GDBusProxy *proxy) {
    return int_prop(proxy, "AccountType") == XD_ACCOUNT_ADMIN;
}

static GDBusProxy *find_account(ua *user_accounts, const char *path) {
    for (guint i = 0; path != nullptr && i < user_accounts->accounts->len; i++) {
        GDBusProxy *proxy = g_ptr_array_index(user_accounts->accounts, i);
        if (strcmp(g_dbus_proxy_get_object_path(proxy), path) == 0)
            return proxy;
    }
    return nullptr;
}

static bool is_me(ua *user_accounts, GDBusProxy *proxy) {
    return user_accounts->my_path != nullptr &&
           strcmp(g_dbus_proxy_get_object_path(proxy), user_accounts->my_path) == 0;
}

static void error_box(ua *user_accounts, const char *what, GError *error) {
    g_dbus_error_strip_remote_error(error);
    char *message = g_strdup_printf("%s\n\n%s", what, error->message);
    xd_message_box(GTK_WINDOW(user_accounts->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, message);
    g_free(message);
}

/* What to do with a call's reply once it succeeded. */
typedef void (*reply_fn)(struct ua *user_accounts, GVariant *reply);

typedef struct pending_call {
    ua *u;
    char *what; /* the error box's text if it fails */
    reply_fn done;
} pending_call;

static void call_done(GObject *src, GAsyncResult *res, gpointer data) {
    pending_call *pending_reply = data;
    ua *user_accounts = pending_reply->u;
    GError *error = nullptr;
    GVariant *reply_value =
        G_IS_DBUS_PROXY(src) ? g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error)
                             : g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &error);
    if (--user_accounts->calls == 0)
        gtk_widget_set_sensitive(user_accounts->body, TRUE);
    if (reply_value == nullptr) {
        error_box(user_accounts, pending_reply->what, error);
        g_error_free(error);
    } else {
        if (pending_reply->done != nullptr)
            pending_reply->done(user_accounts, reply_value);
        g_variant_unref(reply_value);
    }
    g_free(pending_reply->what);
    g_free(pending_reply);
}

/* Calls AccountsService without waiting: polkit's password prompt may be
 * up for a while, and the window must keep drawing and answering
 * meanwhile. It's greyed until the reply, so the page stays as it was;
 * then `done` runs, or an error box says `what` failed. begin_call's
 * result goes to the call as call_done's data. */
static pending_call *begin_call(ua *user_accounts, const char *what, reply_fn done) {
    pending_call *pending_reply = g_new0(pending_call, 1);
    pending_reply->u = user_accounts;
    pending_reply->what = g_strdup(what);
    pending_reply->done = done;
    if (user_accounts->calls++ == 0)
        gtk_widget_set_sensitive(user_accounts->body, FALSE);
    return pending_reply;
}

static void call(ua *user_accounts, GDBusProxy *proxy, const char *method, GVariant *args,
                 const char *what, reply_fn done) {
    g_dbus_proxy_call(proxy, method, args, G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION,
                      G_MAXINT, nullptr, call_done, begin_call(user_accounts, what, done));
}

static void build_page(ua *user_accounts);
static void adv_fill(ua *user_accounts);

static gboolean rebuild_now(gpointer data) {
    ua *user_accounts = data;
    user_accounts->rebuild_idle = 0;
    if (user_accounts->advanced) {
        adv_fill(user_accounts);
        return G_SOURCE_REMOVE;
    }
    /* The account may be gone (deleted elsewhere). */
    if (user_accounts->cur.user != nullptr &&
        find_account(user_accounts, user_accounts->cur.user) == nullptr) {
        g_clear_pointer(&user_accounts->cur.user, g_free);
        user_accounts->cur.kind = PAGE_HOME;
    }
    build_page(user_accounts);
    return G_SOURCE_REMOVE;
}

/* Pages that only show accounts are rebuilt when an account changes;
 * forms aren't, so nothing typed is lost. */
static void account_changed(ua *user_accounts) {
    bool showing = user_accounts->advanced || user_accounts->cur.kind == PAGE_HOME ||
                   user_accounts->cur.kind == PAGE_ACCOUNT;
    if (showing && user_accounts->rebuild_idle == 0)
        user_accounts->rebuild_idle = g_idle_add(rebuild_now, user_accounts);
}

static void on_user_signal(GDBusProxy *proxy, [[maybe_unused]] const char *sender,
                           const char *signal, [[maybe_unused]] GVariant *params, gpointer data);

static GDBusProxy *open_account(ua *user_accounts, const char *path) {
    GDBusProxy *proxy = g_dbus_proxy_new_sync(g_dbus_proxy_get_connection(user_accounts->manager),
                                              G_DBUS_PROXY_FLAGS_NONE, nullptr, ACCOUNTS, path,
                                              USER_IFACE, nullptr, nullptr);
    if (proxy != nullptr) {
        g_signal_connect(proxy, "g-signal", G_CALLBACK(on_user_signal), user_accounts);
        g_signal_connect_swapped(proxy, "g-properties-changed", G_CALLBACK(account_changed),
                                 user_accounts);
    }
    return proxy;
}

static int compare_accounts(gconstpointer left, gconstpointer right) {
    GDBusProxy *left_account = *(GDBusProxy *const *)left,
               *right_account = *(GDBusProxy *const *)right;
    char *nx = display_name(left_account), *ny = display_name(right_account);
    int order = g_utf8_collate(nx, ny);
    g_free(nx);
    g_free(ny);
    return order;
}

static void load_accounts(ua *user_accounts) {
    g_ptr_array_set_size(user_accounts->accounts, 0);
    g_clear_pointer(&user_accounts->my_path, g_free);
    if (user_accounts->manager == nullptr)
        return;
    GVariant *reply_value =
        g_dbus_proxy_call_sync(user_accounts->manager, "ListCachedUsers", nullptr,
                               G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
    if (reply_value == nullptr)
        return;
    GVariantIter *it = nullptr;
    const char *path = nullptr;
    g_variant_get(reply_value, "(ao)", &it);
    while (g_variant_iter_loop(it, "&o", &path)) {
        GDBusProxy *proxy = open_account(user_accounts, path);
        if (proxy == nullptr)
            continue;
        if (bool_prop(proxy, "SystemAccount")) {
            g_object_unref(proxy);
            continue;
        }
        if (uid_prop(proxy) == (guint64)getuid())
            user_accounts->my_path = g_strdup(path);
        g_ptr_array_add(user_accounts->accounts, proxy);
    }
    g_variant_iter_free(it);
    g_variant_unref(reply_value);
    g_ptr_array_sort(user_accounts->accounts, compare_accounts);
}

/* Older AccountsService only says "Changed", without the properties:
 * read the account again. */
static void on_user_signal(GDBusProxy *proxy, [[maybe_unused]] const char *sender,
                           const char *signal, [[maybe_unused]] GVariant *params, gpointer data) {
    ua *user_accounts = data;
    if (strcmp(signal, "Changed") != 0)
        return;
    for (guint i = 0; i < user_accounts->accounts->len; i++) {
        if (g_ptr_array_index(user_accounts->accounts, i) != proxy)
            continue;
        GDBusProxy *fresh = open_account(user_accounts, g_dbus_proxy_get_object_path(proxy));
        if (fresh != nullptr) {
            /* The old proxy is still emitting; the array's reference is
             * dropped once it's done. */
            g_signal_handlers_disconnect_by_data(proxy, user_accounts);
            g_ptr_array_index(user_accounts->accounts, i) = fresh;
            g_idle_add_once((GSourceOnceFunc)g_object_unref, proxy);
        }
        break;
    }
    g_ptr_array_sort(user_accounts->accounts, compare_accounts);
    account_changed(user_accounts);
}

static void on_manager_signal([[maybe_unused]] GDBusProxy *proxy,
                              [[maybe_unused]] const char *sender, const char *signal,
                              [[maybe_unused]] GVariant *params, gpointer data) {
    ua *user_accounts = data;
    if (strcmp(signal, "UserAdded") == 0 || strcmp(signal, "UserDeleted") == 0) {
        load_accounts(user_accounts);
        account_changed(user_accounts);
    }
}

/* ---- navigation ------------------------------------------------------------------ */

static void go(ua *user_accounts, page_kind kind, const char *user) {
    g_array_append_val(user_accounts->history, user_accounts->cur);
    user_accounts->cur = (nav){kind, g_strdup(user)};
    build_page(user_accounts);
}

static void go_back(ua *user_accounts) {
    if (user_accounts->history->len == 0)
        return;
    g_free(user_accounts->cur.user);
    user_accounts->cur =
        g_array_index(user_accounts->history, nav, user_accounts->history->len - 1);
    g_array_set_size(user_accounts->history, user_accounts->history->len - 1);
    build_page(user_accounts);
}

static void clear_history(ua *user_accounts) {
    for (guint i = 0; i < user_accounts->history->len; i++)
        g_free(g_array_index(user_accounts->history, nav, i).user);
    g_array_set_size(user_accounts->history, 0);
}

/* Home, forgetting the way there (after an account is created or
 * deleted, Back has nowhere sensible to go). */
static void go_home(ua *user_accounts) {
    clear_history(user_accounts);
    g_free(user_accounts->cur.user);
    user_accounts->cur = (nav){PAGE_HOME, nullptr};
    build_page(user_accounts);
}

/* Back to the account's page after a change, as XP does. */
static void done(ua *user_accounts) {
    while (user_accounts->history->len > 0 && user_accounts->cur.kind != PAGE_ACCOUNT &&
           user_accounts->cur.kind != PAGE_HOME) {
        g_free(user_accounts->cur.user);
        user_accounts->cur =
            g_array_index(user_accounts->history, nav, user_accounts->history->len - 1);
        g_array_set_size(user_accounts->history, user_accounts->history->len - 1);
    }
    build_page(user_accounts);
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
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 60);
    if (class != nullptr)
        gtk_style_context_add_class(gtk_widget_get_style_context(widget), class);
    return widget;
}

static void add_class(GtkWidget *widget, const char *class) {
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), class);
}

/* A task: a green arrow and a blue link, as on XP's task pages. */
static GtkWidget *task(const char *text, GCallback cb, gpointer data) {
    GtkWidget *link = gtk_button_new();
    add_class(link, "xd-ua-link");
    gtk_button_set_relief(GTK_BUTTON(link), GTK_RELIEF_NONE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon = gtk_image_new_from_icon_name("go-next", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    GtkWidget *caption = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(caption), 0);
    gtk_box_pack_start(GTK_BOX(row), caption, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(link), row);
    gtk_widget_set_halign(link, GTK_ALIGN_START);
    g_signal_connect_swapped(link, "clicked", cb, data);
    return link;
}

/* The account picture: AccountsService's, else a generic one. */
static GtkWidget *picture(GDBusProxy *proxy, int size) {
    char *file = proxy != nullptr ? str_prop(proxy, "IconFile") : g_strdup("");
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

static void unwrap(GtkWidget *widget, [[maybe_unused]] gpointer data) {
    gtk_label_set_line_wrap(GTK_LABEL(widget), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 24);
}

/* Picture, name, account type, password: XP's account tile. */
static GtkWidget *tile_content(GDBusProxy *proxy) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(row), picture(proxy, 48), FALSE, FALSE, 0);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    char *name = display_name(proxy);
    char *markup = g_markup_printf_escaped("<b>%s</b>", name);
    GtkWidget *bold_name = label("", nullptr);
    gtk_label_set_markup(GTK_LABEL(bold_name), markup);
    gtk_box_pack_start(GTK_BOX(col), bold_name, FALSE, FALSE, 0);
    gtk_box_pack_start(
        GTK_BOX(col),
        label(is_admin(proxy) ? "Computer administrator" : "Limited account", "xd-ua-dim"), FALSE,
        FALSE, 0);
    if (bool_prop(proxy, "Locked"))
        gtk_box_pack_start(GTK_BOX(col), label("Account disabled", "xd-ua-dim"), FALSE, FALSE, 0);
    else if (int_prop(proxy, "PasswordMode") == 0)
        gtk_box_pack_start(GTK_BOX(col), label("Password protected", "xd-ua-dim"), FALSE, FALSE,
                           0);
    /* One line each, so tiles stay narrow enough for two a row. */
    gtk_container_foreach(GTK_CONTAINER(col), unwrap, nullptr);
    gtk_box_pack_start(GTK_BOX(row), col, FALSE, FALSE, 0);
    g_free(markup);
    g_free(name);
    return row;
}

static void on_tile(GtkButton *button, gpointer data) {
    go(data, PAGE_ACCOUNT, g_object_get_data(G_OBJECT(button), "path"));
}

static GtkWidget *tile(ua *user_accounts, GDBusProxy *proxy) {
    GtkWidget *widget = gtk_button_new();
    add_class(widget, "xd-ua-tile");
    gtk_button_set_relief(GTK_BUTTON(widget), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(widget), tile_content(proxy));
    gtk_widget_set_size_request(widget, 230, -1);
    g_object_set_data_full(G_OBJECT(widget), "path", g_strdup(g_dbus_proxy_get_object_path(proxy)),
                           g_free);
    g_signal_connect(widget, "clicked", G_CALLBACK(on_tile), user_accounts);
    return widget;
}

/* OK-style buttons at the bottom right of a form. */
static GtkWidget *button(const char *text, GCallback cb, gpointer data) {
    GtkWidget *widget = gtk_button_new_with_mnemonic(text);
    gtk_widget_set_size_request(widget, 90, -1);
    g_signal_connect_swapped(widget, "clicked", cb, data);
    return widget;
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

static GtkWidget *entry(ua *user_accounts, int i, const char *text, bool password) {
    GtkWidget *widget = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(widget), text != nullptr ? text : "");
    gtk_entry_set_visibility(GTK_ENTRY(widget), !password);
    gtk_entry_set_activates_default(GTK_ENTRY(widget), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(widget), 32);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    user_accounts->entry[i] = widget;
    return widget;
}

static const char *entry_text(ua *user_accounts, int i) {
    return gtk_entry_get_text(GTK_ENTRY(user_accounts->entry[i]));
}

/* A blue box in the task pane: a title and links. */
static GtkWidget *pane_box(const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_class(box, "xd-ua-box");
    GtkWidget *widget = label(title, "xd-ua-box-title");
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    return box;
}

static void pane_add(GtkWidget *box, GtkWidget *link) {
    gtk_widget_set_margin_start(link, 6);
    gtk_box_pack_start(GTK_BOX(box), link, FALSE, FALSE, 0);
}

/* ---- actions --------------------------------------------------------------------- */

static GDBusProxy *cur_account(ua *user_accounts) {
    return find_account(user_accounts, user_accounts->cur.user);
}

static void run_change_password(ua *user_accounts) {
    /* Our own Change Password (xd-security), next to us or on PATH. */
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    char *dir = self != nullptr ? g_path_get_dirname(self) : g_strdup(".");
    char *local = g_build_filename(dir, "xd-security", nullptr);
    const char *argv[] = {g_file_test(local, G_FILE_TEST_IS_EXECUTABLE) ? local : "xd-security",
                          "--change-password", nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        error_box(user_accounts, "Change Password could not be started.", error);
        g_error_free(error);
    }
    g_free(local);
    g_free(dir);
    g_free(self);
}

static void changed(ua *user_accounts, [[maybe_unused]] GVariant *reply) { done(user_accounts); }

static void on_change_name(ua *user_accounts) {
    GDBusProxy *proxy = cur_account(user_accounts);
    char *name = g_strstrip(g_strdup(entry_text(user_accounts, 0)));
    if (proxy != nullptr)
        call(user_accounts, proxy, "SetRealName", g_variant_new("(s)", name),
             "The name could not be changed.", changed);
    g_free(name);
}

/* The picture for `proxy`, made from `source`. Our own goes to ~/.face, which
 * the Start menu (and other desktops' greeters) read; another account's
 * stays in our cache. AccountsService copies it from there, but keeps the
 * name it was given when it can't (as python-dbusmock does), so the file
 * stays. */
static void set_picture(ua *user_accounts, GDBusProxy *proxy, const char *source) {
    char *path;
    if (is_me(user_accounts, proxy)) {
        path = g_build_filename(g_get_home_dir(), ".face", nullptr);
    } else {
        char *dir = g_build_filename(g_get_user_cache_dir(), "xeondesktop", nullptr);
        g_mkdir_with_parents(dir, 0700);
        char *name = g_strdup_printf("account-picture-%" G_GUINT64_FORMAT ".png", uid_prop(proxy));
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
        error_box(user_accounts, "This picture can't be used.", error);
        g_error_free(error);
        g_unlink(tmp);
    } else {
        call(user_accounts, proxy, "SetIconFile", g_variant_new("(s)", path),
             "The picture could not be changed.", changed);
    }
    g_free(tmp);
    g_free(path);
}

static void on_change_picture(ua *user_accounts) {
    GDBusProxy *proxy = cur_account(user_accounts);
    if (proxy != nullptr && user_accounts->picture != nullptr)
        set_picture(user_accounts, proxy, user_accounts->picture);
}

static void on_browse(ua *user_accounts) {
    GtkWidget *widget = gtk_file_chooser_dialog_new(
        "Open", GTK_WINDOW(user_accounts->window), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
        GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter *pictures = gtk_file_filter_new();
    gtk_file_filter_set_name(pictures, "Pictures (*.bmp;*.jpg;*.png;*.webp)");
    gtk_file_filter_add_pixbuf_formats(pictures);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(widget), pictures);
    const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (pics != nullptr)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(widget), pics);
    char *file = gtk_dialog_run(GTK_DIALOG(widget)) == GTK_RESPONSE_ACCEPT
                     ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget))
                     : nullptr;
    gtk_widget_destroy(widget);
    GDBusProxy *proxy = cur_account(user_accounts);
    if (file != nullptr && proxy != nullptr)
        set_picture(user_accounts, proxy, file);
    g_free(file);
}

static int count_admins(ua *user_accounts) {
    int count = 0;
    for (guint i = 0; i < user_accounts->accounts->len; i++)
        count += is_admin(g_ptr_array_index(user_accounts->accounts, i)) ? 1 : 0;
    return count;
}

static void on_change_type(ua *user_accounts) {
    GDBusProxy *proxy = cur_account(user_accounts);
    if (proxy == nullptr)
        return;
    bool admin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(user_accounts->admin));
    if (admin == is_admin(proxy)) {
        done(user_accounts);
        return;
    }
    if (!admin && count_admins(user_accounts) <= 1) {
        xd_message_box(GTK_WINDOW(user_accounts->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "This is the only computer administrator account. At least one account "
                       "must be an administrator, so it can't be changed to a limited account.");
        return;
    }
    call(user_accounts, proxy, "SetAccountType",
         g_variant_new("(i)", admin ? XD_ACCOUNT_ADMIN : XD_ACCOUNT_STANDARD),
         "The account type could not be changed.", changed);
}

static void on_set_password(ua *user_accounts) {
    GDBusProxy *proxy = cur_account(user_accounts);
    if (proxy == nullptr)
        return;
    const char *pw = entry_text(user_accounts, 0), *again = entry_text(user_accounts, 1);
    if (strcmp(pw, again) != 0) {
        xd_message_box(GTK_WINDOW(user_accounts->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "The passwords you typed do not match. Type the new password in both "
                       "text boxes.");
        return;
    }
    if (pw[0] == '\0') {
        xd_message_box(GTK_WINDOW(user_accounts->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "Type a password.");
        return;
    }
    char *hash = xd_users_hash_password(pw);
    if (hash == nullptr) {
        xd_message_box(GTK_WINDOW(user_accounts->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The password could not be encrypted.");
        return;
    }
    call(user_accounts, proxy, "SetPassword",
         g_variant_new("(ss)", hash, entry_text(user_accounts, 2)),
         "The password could not be changed.", changed);
    explicit_bzero(hash, strlen(hash));
    g_free(hash);
}

static void on_delete_files(ua *user_accounts) {
    user_accounts->keep_files = false;
    go(user_accounts, PAGE_DELETE_CONFIRM, user_accounts->cur.user);
}

static void on_keep_files(ua *user_accounts) {
    user_accounts->keep_files = true;
    go(user_accounts, PAGE_DELETE_CONFIRM, user_accounts->cur.user);
}

static void deleted(ua *user_accounts, [[maybe_unused]] GVariant *reply) {
    load_accounts(user_accounts);
    go_home(user_accounts);
}

static void on_delete(ua *user_accounts) {
    GDBusProxy *proxy = cur_account(user_accounts);
    if (proxy == nullptr || user_accounts->manager == nullptr)
        return;
    xd_accounts_delete_user(
        g_dbus_proxy_get_connection(user_accounts->manager), (gint64)uid_prop(proxy),
        !user_accounts->keep_files, nullptr, call_done,
        begin_call(user_accounts, "The account could not be deleted.", deleted));
}

static void on_full_name_changed([[maybe_unused]] GtkEditable *editable, gpointer data) {
    ua *user_accounts = data;
    if (user_accounts->name_edited)
        return;
    char *suggestion = xd_users_suggest_name(entry_text(user_accounts, 0));
    user_accounts->suggesting = true;
    gtk_entry_set_text(GTK_ENTRY(user_accounts->entry[1]), suggestion);
    user_accounts->suggesting = false;
    g_free(suggestion);
}

static void on_user_name_changed([[maybe_unused]] GtkEditable *editable, gpointer data) {
    ua *user_accounts = data;
    if (!user_accounts->suggesting)
        user_accounts->name_edited = true;
}

static void on_create_next(ua *user_accounts) {
    char *full = g_strstrip(g_strdup(entry_text(user_accounts, 0)));
    const char *name = entry_text(user_accounts, 1);
    if (!xd_users_valid_name(name)) {
        xd_message_box(GTK_WINDOW(user_accounts->window), TITLE, XD_MB_WARNING, XD_MB_OK, FALSE,
                       "The user name must start with a lowercase letter and contain only "
                       "lowercase letters, digits, hyphens and underscores (at most 32).");
        g_free(full);
        return;
    }
    g_free(user_accounts->new_full);
    g_free(user_accounts->new_name);
    user_accounts->new_full = full;
    user_accounts->new_name = g_strdup(name);
    go(user_accounts, PAGE_CREATE_TYPE, nullptr);
}

static void created(ua *user_accounts, GVariant *reply_value) {
    const char *path = nullptr;
    g_variant_get(reply_value, "(&o)", &path);
    load_accounts(user_accounts);
    /* On to the new account, where it can get a password and picture. */
    clear_history(user_accounts);
    g_free(user_accounts->cur.user);
    user_accounts->cur = (nav){PAGE_HOME, nullptr};
    if (find_account(user_accounts, path) != nullptr)
        go(user_accounts, PAGE_ACCOUNT, path);
    else
        build_page(user_accounts);
}

static void on_create(ua *user_accounts) {
    bool admin = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(user_accounts->admin));
    call(user_accounts, user_accounts->manager, "CreateUser",
         g_variant_new("(ssi)", user_accounts->new_name, user_accounts->new_full,
                       admin ? XD_ACCOUNT_ADMIN : XD_ACCOUNT_STANDARD),
         "The account could not be created.", created);
}

static void on_go_name(ua *user_accounts) { go(user_accounts, PAGE_NAME, user_accounts->cur.user); }
static void on_go_picture(ua *user_accounts) {
    go(user_accounts, PAGE_PICTURE, user_accounts->cur.user);
}
static void on_go_type(ua *user_accounts) { go(user_accounts, PAGE_TYPE, user_accounts->cur.user); }
static void on_go_delete(ua *user_accounts) {
    go(user_accounts, PAGE_DELETE, user_accounts->cur.user);
}
static void on_go_create(ua *user_accounts) {
    g_clear_pointer(&user_accounts->new_full, g_free);
    g_clear_pointer(&user_accounts->new_name, g_free);
    go(user_accounts, PAGE_CREATE_NAME, nullptr);
}
static void on_go_change(ua *user_accounts) { go_home(user_accounts); }
static void on_go_mine(ua *user_accounts) {
    go(user_accounts, PAGE_ACCOUNT, user_accounts->my_path);
}

static void on_go_password(ua *user_accounts) {
    GDBusProxy *proxy = cur_account(user_accounts);
    if (proxy != nullptr && is_me(user_accounts, proxy))
        run_change_password(user_accounts);
    else
        go(user_accounts, PAGE_PASSWORD, user_accounts->cur.user);
}

static void on_profiles(ua *user_accounts) {
    const char *argv[] = {"xd-sysdm", "--page", "3", nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        error_box(user_accounts, "System Properties could not be started.", error);
        g_error_free(error);
    }
}

/* XP's "Manage my network passwords" opened Stored User Names and
 * Passwords; here it's Passwords and Keys. */
static void on_passwords(ua *user_accounts) {
    const char *argv[] = {"xd-keys", nullptr};
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, (char **)(uintptr_t)argv, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &error)) {
        error_box(user_accounts, "Passwords and Keys could not be started.", error);
        g_error_free(error);
    }
}

/* ---- pages ------------------------------------------------------------------------- */

static void page_home(ua *user_accounts, GtkWidget *box) {
    gtk_box_pack_start(GTK_BOX(box), label("Pick a task...", "xd-ua-title"), FALSE, FALSE, 0);
    GtkWidget *tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(tasks, 20);
    if (user_accounts->my_path != nullptr)
        gtk_box_pack_start(GTK_BOX(tasks),
                           task("Change my account", G_CALLBACK(on_go_mine), user_accounts), FALSE,
                           FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tasks),
                       task("Create a new account", G_CALLBACK(on_go_create), user_accounts), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), tasks, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), label("or pick an account to change", "xd-ua-title"), FALSE,
                       FALSE, 12);
    /* Two a row, as in XP. */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 20);
    for (guint i = 0; i < user_accounts->accounts->len; i++)
        gtk_grid_attach(GTK_GRID(grid),
                        tile(user_accounts, g_ptr_array_index(user_accounts->accounts, i)),
                        (int)(i % 2), (int)(i / 2), 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    if (user_accounts->manager == nullptr)
        gtk_box_pack_start(GTK_BOX(box),
                           label("Accounts can't be shown: AccountsService isn't available.",
                                 nullptr),
                           FALSE, FALSE, 0);
}

static void page_account(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    bool me = is_me(user_accounts, proxy);
    char *name = display_name(proxy);
    char *title = me ? g_strdup("What do you want to change about your account?")
                     : g_strdup_printf("What do you want to change about %s's account?", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    GtkWidget *tasks = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(tasks, 20);
    bool has_password = int_prop(proxy, "PasswordMode") == 0 && !bool_prop(proxy, "Locked");
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
                               task(me ? tasks_list[i].mine : tasks_list[i].other, tasks_list[i].cb,
                                    user_accounts),
                               FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), tasks, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), tile_content(proxy), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    g_free(title);
    g_free(name);
}

static void page_name(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    char *name = display_name(proxy);
    char *title = is_me(user_accounts, proxy)
                      ? g_strdup("Provide a new name for your account")
                      : g_strdup_printf("Provide a new name for %s's account", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       label("Type a new name. It appears on the Start menu and the Welcome "
                             "screen.",
                             nullptr),
                       FALSE, FALSE, 0);
    char *real = str_prop(proxy, "RealName");
    gtk_box_pack_start(GTK_BOX(box), entry(user_accounts, 0, real, false), FALSE, FALSE, 0);
    user_accounts->ok = button("Change _Name", G_CALLBACK(on_change_name), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_back), user_accounts), nullptr),
                     FALSE, FALSE, 0);
    g_free(real);
    g_free(title);
    g_free(name);
}

static void on_picture_selected(GtkFlowBox *flow_box, gpointer data) {
    ua *user_accounts = data;
    GList *sel = gtk_flow_box_get_selected_children(flow_box);
    g_free(user_accounts->picture);
    user_accounts->picture =
        sel != nullptr ? g_strdup(g_object_get_data(sel->data, "file")) : nullptr;
    g_list_free(sel);
    gtk_widget_set_sensitive(user_accounts->ok, user_accounts->picture != nullptr);
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
    GDir *directory = g_dir_open(dir, 0, nullptr);
    if (directory == nullptr)
        return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (const char *entry_name = g_dir_read_name(directory); entry_name != nullptr;
         entry_name = g_dir_read_name(directory))
        g_ptr_array_add(names, g_build_filename(dir, entry_name, nullptr));
    g_dir_close(directory);
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

static void page_picture(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    char *name = display_name(proxy);
    char *title = is_me(user_accounts, proxy)
                      ? g_strdup("Pick a new picture for your account")
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
    for (size_t sub_index = 0; sub_index < XD_ARRAY_LEN(sub); sub_index++) {
        for (const char *const *data_dir = dirs; *data_dir != nullptr; data_dir++) {
            char *dir = g_build_filename(*data_dir, sub[sub_index], nullptr);
            find_pictures(dir, 1, files);
            g_free(dir);
        }
    }
    user_accounts->flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(user_accounts->flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(user_accounts->flow), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(user_accounts->flow), 8);
    gtk_widget_set_valign(user_accounts->flow, GTK_ALIGN_START);
    for (guint i = 0; i < files->len; i++) {
        const char *file = g_ptr_array_index(files, i);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(file, 56, 56, TRUE, nullptr);
        if (pb == nullptr)
            continue;
        GtkWidget *img = gtk_image_new_from_pixbuf(pb);
        g_object_unref(pb);
        char *base = g_path_get_basename(file);
        gtk_widget_set_tooltip_text(img, base);
        g_free(base);
        gtk_container_add(GTK_CONTAINER(user_accounts->flow), img);
        GtkWidget *child = gtk_widget_get_parent(img);
        g_object_set_data_full(G_OBJECT(child), "file", g_strdup(file), g_free);
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
    gtk_container_add(GTK_CONTAINER(sc), user_accounts->flow);
    gtk_box_pack_start(GTK_BOX(box), sc, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       task("Browse for more pictures", G_CALLBACK(on_browse), user_accounts),
                       FALSE, FALSE, 0);
    g_clear_pointer(&user_accounts->picture, g_free);
    user_accounts->ok = button("Change _Picture", G_CALLBACK(on_change_picture), user_accounts);
    gtk_widget_set_sensitive(user_accounts->ok, FALSE);
    g_signal_connect(user_accounts->flow, "selected-children-changed",
                     G_CALLBACK(on_picture_selected), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_back), user_accounts), nullptr),
                     FALSE, FALSE, 0);
    g_free(title);
    g_free(name);
}

/* The two account types, as XP explains them. */
static void type_choice(ua *user_accounts, GtkWidget *box, bool admin) {
    GtkWidget *admin_radio = gtk_radio_button_new_with_mnemonic(nullptr, "Computer _administrator");
    GtkWidget *limited_radio =
        gtk_radio_button_new_with_mnemonic_from_widget(GTK_RADIO_BUTTON(admin_radio), "_Limited");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(admin ? admin_radio : limited_radio), TRUE);
    user_accounts->admin = admin_radio;
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 30);
    gtk_box_pack_start(GTK_BOX(row), admin_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), limited_radio, FALSE, FALSE, 0);
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

static void page_type(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    char *name = display_name(proxy);
    char *title = is_me(user_accounts, proxy)
                      ? g_strdup("Pick a new account type for your account")
                      : g_strdup_printf("Pick a new account type for %s", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    type_choice(user_accounts, box, is_admin(proxy));
    user_accounts->ok = button("Change Account _Type", G_CALLBACK(on_change_type), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_back), user_accounts), nullptr),
                     FALSE, FALSE, 0);
    g_free(title);
    g_free(name);
}

static GtkWidget *form_row(GtkWidget *box, const char *text, GtkWidget *field) {
    GtkWidget *widget = label(text, nullptr);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), field, FALSE, FALSE, 0);
    return field;
}

static void page_password(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    char *name = display_name(proxy);
    bool has = int_prop(proxy, "PasswordMode") == 0 && !bool_prop(proxy, "Locked");
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
    form_row(box, "Type a new password:", entry(user_accounts, 0, "", true));
    form_row(box, "Type the new password again to confirm:", entry(user_accounts, 1, "", true));
    form_row(box,
             "Type a word or phrase to use as a password hint (everyone who uses this computer "
             "can see it):",
             entry(user_accounts, 2, "", false));
    user_accounts->ok = button(has ? "Change _Password" : "Create _Password",
                               G_CALLBACK(on_set_password), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_back), user_accounts), nullptr),
                     FALSE, FALSE, 0);
    g_free(warn);
    g_free(title);
    g_free(name);
}

static void page_delete(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    char *name = display_name(proxy);
    char *title = g_strdup_printf("Do you want to keep %s's files?", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    char *home = str_prop(proxy, "HomeDirectory");
    char *text = g_strdup_printf(
        "Before you delete %s's account, you can keep the account's home folder (%s), with "
        "its desktop, documents, pictures and settings, for an administrator to look "
        "through. Or you can delete them along with the account.",
        name, home);
    gtk_box_pack_start(GTK_BOX(box), label(text, nullptr), FALSE, FALSE, 0);
    user_accounts->ok = button("_Keep Files", G_CALLBACK(on_keep_files), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("_Delete Files", G_CALLBACK(on_delete_files), user_accounts),
                                button("Cancel", G_CALLBACK(go_back), user_accounts)),
                     FALSE, FALSE, 0);
    g_free(text);
    g_free(home);
    g_free(title);
    g_free(name);
}

static void page_delete_confirm(ua *user_accounts, GtkWidget *box, GDBusProxy *proxy) {
    char *name = display_name(proxy);
    char *title = g_strdup_printf("Are you sure you want to delete %s's account?", name);
    gtk_box_pack_start(GTK_BOX(box), label(title, "xd-ua-title"), FALSE, FALSE, 0);
    char *text = g_strdup_printf(user_accounts->keep_files
                                     ? "%s's home folder will be kept, and the account deleted."
                                     : "All of %s's files, and the account, will be deleted.",
                                 name);
    gtk_box_pack_start(GTK_BOX(box), label(text, nullptr), FALSE, FALSE, 0);
    user_accounts->ok = button("_Delete Account", G_CALLBACK(on_delete), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_back), user_accounts), nullptr),
                     FALSE, FALSE, 0);
    g_free(text);
    g_free(title);
    g_free(name);
}

static void page_create_name(ua *user_accounts, GtkWidget *box) {
    gtk_box_pack_start(GTK_BOX(box), label("Name the new account", "xd-ua-title"), FALSE, FALSE,
                       0);
    form_row(box, "Type a name for the new account:",
             entry(user_accounts, 0, user_accounts->new_full, false));
    gtk_box_pack_start(GTK_BOX(box),
                       label("This name will appear on the Welcome screen and on the Start "
                             "menu.",
                             "xd-ua-dim"),
                       FALSE, FALSE, 0);
    form_row(box,
             "User name, for logging on:", entry(user_accounts, 1, user_accounts->new_name, false));
    user_accounts->name_edited = user_accounts->new_name != nullptr;
    g_signal_connect(user_accounts->entry[0], "changed", G_CALLBACK(on_full_name_changed),
                     user_accounts);
    g_signal_connect(user_accounts->entry[1], "changed", G_CALLBACK(on_user_name_changed),
                     user_accounts);
    user_accounts->ok = button("_Next >", G_CALLBACK(on_create_next), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_home), user_accounts), nullptr),
                     FALSE, FALSE, 0);
}

static void page_create_type(ua *user_accounts, GtkWidget *box) {
    gtk_box_pack_start(GTK_BOX(box), label("Pick an account type", "xd-ua-title"), FALSE, FALSE,
                       0);
    type_choice(user_accounts, box, false);
    user_accounts->ok = button("_Create Account", G_CALLBACK(on_create), user_accounts);
    gtk_box_pack_end(GTK_BOX(box),
                     button_row(button("< _Back", G_CALLBACK(go_back), user_accounts),
                                user_accounts->ok,
                                button("Cancel", G_CALLBACK(go_home), user_accounts)),
                     FALSE, FALSE, 0);
}

static void build_pane(ua *user_accounts) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(user_accounts->pane));
    for (GList *list = kids; list != nullptr; list = list->next)
        gtk_widget_destroy(list->data);
    g_list_free(kids);
    GtkWidget *related = pane_box("Related Tasks");
    if (user_accounts->cur.kind != PAGE_HOME)
        pane_add(related, task("Change another account", G_CALLBACK(on_go_change), user_accounts));
    pane_add(related, task("Create a new account", G_CALLBACK(on_go_create), user_accounts));
    GDBusProxy *cur = cur_account(user_accounts);
    if (user_accounts->cur.kind == PAGE_ACCOUNT && cur != nullptr && is_me(user_accounts, cur))
        pane_add(related,
                 task("Manage my network passwords", G_CALLBACK(on_passwords), user_accounts));
    gtk_box_pack_start(GTK_BOX(user_accounts->pane), related, FALSE, FALSE, 0);
    GtkWidget *see = pane_box("See Also");
    pane_add(see, task("User profiles", G_CALLBACK(on_profiles), user_accounts));
    gtk_box_pack_start(GTK_BOX(user_accounts->pane), see, FALSE, FALSE, 0);
    gtk_widget_show_all(user_accounts->pane);
}

static void build_page(ua *user_accounts) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(user_accounts->main));
    for (GList *list = kids; list != nullptr; list = list->next)
        gtk_widget_destroy(list->data);
    g_list_free(kids);
    for (size_t i = 0; i < XD_ARRAY_LEN(user_accounts->entry); i++)
        user_accounts->entry[i] = nullptr;
    user_accounts->admin = user_accounts->flow = user_accounts->ok = nullptr;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);
    GDBusProxy *proxy = cur_account(user_accounts);
    if (user_accounts->cur.kind != PAGE_HOME && user_accounts->cur.kind < PAGE_CREATE_NAME &&
        proxy == nullptr)
        user_accounts->cur.kind = PAGE_HOME;
    switch (user_accounts->cur.kind) {
    case PAGE_HOME:
        page_home(user_accounts, box);
        break;
    case PAGE_ACCOUNT:
        page_account(user_accounts, box, proxy);
        break;
    case PAGE_NAME:
        page_name(user_accounts, box, proxy);
        break;
    case PAGE_PICTURE:
        page_picture(user_accounts, box, proxy);
        break;
    case PAGE_TYPE:
        page_type(user_accounts, box, proxy);
        break;
    case PAGE_PASSWORD:
        page_password(user_accounts, box, proxy);
        break;
    case PAGE_DELETE:
        page_delete(user_accounts, box, proxy);
        break;
    case PAGE_DELETE_CONFIRM:
        page_delete_confirm(user_accounts, box, proxy);
        break;
    case PAGE_CREATE_NAME:
        page_create_name(user_accounts, box);
        break;
    case PAGE_CREATE_TYPE:
        page_create_type(user_accounts, box);
        break;
    }
    gtk_box_pack_start(GTK_BOX(user_accounts->main), box, TRUE, TRUE, 0);
    build_pane(user_accounts);
    gtk_widget_set_sensitive(user_accounts->back, user_accounts->history->len > 0);
    gtk_widget_show_all(user_accounts->main);
    if (user_accounts->ok != nullptr) {
        gtk_widget_set_can_default(user_accounts->ok, TRUE);
        gtk_widget_grab_default(user_accounts->ok);
    }
    if (user_accounts->entry[0] != nullptr)
        gtk_widget_grab_focus(user_accounts->entry[0]);
}

/* ---- advanced (control userpasswords2) ------------------------------------------ */

/* XP's other User Accounts, the one `control userpasswords2` opens: here
 * only its Users tab's "Users must enter a user name and password" (off
 * is automatic logon, AccountsService's AutomaticLogin) and the list of
 * users. */

enum { ADV_NAME, ADV_GROUP, ADV_PATH, ADV_NCOLS };

static bool anyone_logs_on_automatically(ua *user_accounts) {
    for (guint i = 0; i < user_accounts->accounts->len; i++) {
        if (bool_prop(g_ptr_array_index(user_accounts->accounts, i), "AutomaticLogin"))
            return true;
    }
    return false;
}

static void adv_fill(ua *user_accounts) {
    /* Keep the selection across a refresh. */
    char *selected = nullptr;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(user_accounts->users_view));
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, ADV_PATH, &selected, -1);
    gtk_list_store_clear(user_accounts->users);
    for (guint i = 0; i < user_accounts->accounts->len; i++) {
        GDBusProxy *proxy = g_ptr_array_index(user_accounts->accounts, i);
        char *name = str_prop(proxy, "UserName");
        const char *path = g_dbus_proxy_get_object_path(proxy);
        gtk_list_store_insert_with_values(user_accounts->users, &it, -1, ADV_NAME, name, ADV_GROUP,
                                          is_admin(proxy) ? "Administrators" : "Users", ADV_PATH,
                                          path, -1);
        if (g_strcmp0(path, selected != nullptr ? selected : user_accounts->my_path) == 0)
            gtk_tree_selection_select_iter(sel, &it);
        g_free(name);
    }
    g_free(selected);
}

static void adv_set_dirty(ua *user_accounts, bool dirty) {
    gtk_widget_set_sensitive(user_accounts->apply, dirty);
}

static void on_must_logon_toggled([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    adv_set_dirty(data, true);
}

/* The last change is made: Apply greys, and OK closes. */
static void adv_applied(ua *user_accounts, [[maybe_unused]] GVariant *reply) {
    if (user_accounts->calls > 0)
        return;
    adv_set_dirty(user_accounts, false);
    if (user_accounts->closing)
        gtk_widget_destroy(user_accounts->window);
}

/* XP's Automatically Log On box: which account logs on by itself. XP also
 * asked for its password, to keep in the registry; AccountsService needs
 * none. nullptr if cancelled. */
static GDBusProxy *ask_auto_logon(ua *user_accounts) {
    GtkWidget *logon_dialog = gtk_dialog_new_with_buttons(
        "Automatically Log On", GTK_WINDOW(user_accounts->window), GTK_DIALOG_MODAL, "OK",
        GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(logon_dialog), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(logon_dialog), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(logon_dialog))), box, TRUE,
                       TRUE, 0);
    GtkWidget *text = label("You can set up your computer so that users do not have to type a "
                            "user name and password to log on. To do this, specify a user that "
                            "will be automatically logged on below:",
                            nullptr);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 50);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *caption = gtk_label_new_with_mnemonic("_User name:");
    gtk_box_pack_start(GTK_BOX(row), caption, FALSE, FALSE, 0);
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(caption), combo);
    /* The account selected in the list, as XP filled in. */
    char *selected = nullptr;
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(user_accounts->users_view)), &model, &it))
        gtk_tree_model_get(model, &it, ADV_PATH, &selected, -1);
    for (guint i = 0; i < user_accounts->accounts->len; i++) {
        GDBusProxy *proxy = g_ptr_array_index(user_accounts->accounts, i);
        char *name = str_prop(proxy, "UserName");
        const char *path = g_dbus_proxy_get_object_path(proxy);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), path, name);
        g_free(name);
    }
    if (selected == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), selected))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_free(selected);
    gtk_widget_set_hexpand(combo, TRUE);
    gtk_box_pack_start(GTK_BOX(row), combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_widget_show_all(logon_dialog);
    /* Enter is OK, as in XP's box (on the list, GTK would open it). */
    gtk_widget_grab_focus(
        gtk_dialog_get_widget_for_response(GTK_DIALOG(logon_dialog), GTK_RESPONSE_OK));
    GDBusProxy *who = nullptr;
    if (gtk_dialog_run(GTK_DIALOG(logon_dialog)) == GTK_RESPONSE_OK)
        who = find_account(user_accounts, gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    gtk_widget_destroy(logon_dialog);
    return who;
}

/* Makes the check box's setting so; false if there was nothing to do (or
 * it was cancelled, `cancelled`). */
static bool adv_apply(ua *user_accounts, bool *cancelled) {
    *cancelled = false;
    bool must = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(user_accounts->must_logon));
    if (must == !anyone_logs_on_automatically(user_accounts))
        return false;
    if (!must) {
        GDBusProxy *who = ask_auto_logon(user_accounts);
        if (who == nullptr) {
            *cancelled = true;
            return false;
        }
        /* AccountsService turns it off for whoever had it. */
        call(user_accounts, who, "SetAutomaticLogin", g_variant_new("(b)", TRUE),
             "Automatic logon could not be turned on.", adv_applied);
        return true;
    }
    for (guint i = 0; i < user_accounts->accounts->len; i++) {
        GDBusProxy *proxy = g_ptr_array_index(user_accounts->accounts, i);
        if (bool_prop(proxy, "AutomaticLogin"))
            call(user_accounts, proxy, "SetAutomaticLogin", g_variant_new("(b)", FALSE),
                 "Automatic logon could not be turned off.", adv_applied);
    }
    return true;
}

static void on_adv_response(GtkDialog *dialog, int response, gpointer data) {
    ua *user_accounts = data;
    if (user_accounts->calls > 0)
        return; /* a change is still being made */
    if (response != GTK_RESPONSE_OK && response != GTK_RESPONSE_APPLY) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    user_accounts->closing = response == GTK_RESPONSE_OK;
    bool cancelled = false;
    if (adv_apply(user_accounts, &cancelled))
        return; /* adv_applied finishes */
    if (cancelled) {
        /* As it was: someone still has to log on. */
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(user_accounts->must_logon), TRUE);
        user_accounts->closing = false;
    }
    adv_set_dirty(user_accounts, false);
    if (user_accounts->closing)
        gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void build_advanced(ua *user_accounts) {
    user_accounts->window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(user_accounts->window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(user_accounts->window), "system-users");
    gtk_window_set_resizable(GTK_WINDOW(user_accounts->window), FALSE);
    /* Greyed, buttons and all, while AccountsService makes a change. */
    user_accounts->body = gtk_bin_get_child(GTK_BIN(user_accounts->window));
    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(notebook), 6);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(user_accounts->window))),
                      notebook);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), page, gtk_label_new("Users"));

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("system-users", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), img, FALSE, FALSE, 0);
    GtkWidget *intro = label("Use the list below to see who can use your computer. To change "
                             "their names, pictures and passwords, use User Accounts in Control "
                             "Panel.",
                             nullptr);
    gtk_label_set_max_width_chars(GTK_LABEL(intro), 50);
    gtk_box_pack_start(GTK_BOX(top), intro, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    user_accounts->must_logon = gtk_check_button_new_with_mnemonic(
        "Users _must enter a user name and password to use this computer.");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(user_accounts->must_logon),
                                 !anyone_logs_on_automatically(user_accounts));
    g_signal_connect(user_accounts->must_logon, "toggled", G_CALLBACK(on_must_logon_toggled),
                     user_accounts);
    gtk_widget_set_sensitive(user_accounts->must_logon, user_accounts->manager != nullptr);
    gtk_box_pack_start(GTK_BOX(page), user_accounts->must_logon, FALSE, FALSE, 0);

    GtkWidget *widget = gtk_label_new_with_mnemonic("U_sers for this computer:");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_box_pack_start(GTK_BOX(page), widget, FALSE, FALSE, 0);
    user_accounts->users =
        gtk_list_store_new(ADV_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    user_accounts->users_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(user_accounts->users));
    g_object_unref(user_accounts->users);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), user_accounts->users_view);
    static const char *const titles[] = {"User Name", "Group"};
    for (int i = 0; i < 2; i++) {
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            titles[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(user_accounts->users_view), column);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, 360, 150);
    gtk_container_add(GTK_CONTAINER(scroll), user_accounts->users_view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);
    adv_fill(user_accounts);

    /* Whether the logon screen obeys the check box at all. */
    char *unit =
        user_accounts->manager != nullptr
            ? xd_accounts_display_manager_unit(g_dbus_proxy_get_connection(user_accounts->manager))
            : nullptr;
    char *text = user_accounts->manager != nullptr
                     ? xd_accounts_autologin_note(unit)
                     : g_strdup("Accounts can't be shown: AccountsService isn't available.");
    GtkWidget *note = label(text, nullptr);
    gtk_label_set_max_width_chars(GTK_LABEL(note), 56);
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(page), note, FALSE, FALSE, 0);
    g_free(text);
    g_free(unit);

    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(user_accounts->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(user_accounts->window), "Cancel", GTK_RESPONSE_CANCEL);
    user_accounts->apply =
        gtk_dialog_add_button(GTK_DIALOG(user_accounts->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(user_accounts->window), GTK_RESPONSE_OK);
    adv_set_dirty(user_accounts, false);
    g_signal_connect(user_accounts->window, "response", G_CALLBACK(on_adv_response), user_accounts);
    gtk_widget_show_all(user_accounts->window);
    gtk_widget_grab_focus(ok);
}

/* ---- window ------------------------------------------------------------------------ */

static GtkWidget *toolbar_button(const char *icon, const char *text, GCallback cb,
                                 ua *user_accounts) {
    GtkWidget *widget = gtk_button_new_with_mnemonic(text);
    gtk_button_set_image(GTK_BUTTON(widget),
                         gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
    gtk_button_set_always_show_image(GTK_BUTTON(widget), TRUE);
    gtk_button_set_relief(GTK_BUTTON(widget), GTK_RELIEF_NONE);
    g_signal_connect_swapped(widget, "clicked", cb, user_accounts);
    return widget;
}

static gboolean on_key(GtkWidget *widget, GdkEventKey *key_event, gpointer data) {
    ua *user_accounts = data;
    if (user_accounts->calls > 0)
        return FALSE; /* the page waits for a change */
    /* Alt+Left and Backspace (outside text boxes) go back, as in Explorer. */
    bool text = GTK_IS_ENTRY(gtk_window_get_focus(GTK_WINDOW(widget)));
    if ((key_event->keyval == GDK_KEY_Left && (key_event->state & GDK_MOD1_MASK)) ||
        (key_event->keyval == GDK_KEY_BackSpace && !text)) {
        go_back(user_accounts);
        return TRUE;
    }
    return FALSE;
}

/* The task pages' window: Back and Home, the blue task pane and the page. */
static void build_window(ua *user_accounts) {
    user_accounts->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(user_accounts->window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(user_accounts->window), "system-users");
    gtk_window_set_default_size(GTK_WINDOW(user_accounts->window), 720, 520);
    g_signal_connect(user_accounts->window, "key-press-event", G_CALLBACK(on_key), user_accounts);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(user_accounts->window), outer);
    user_accounts->body = outer;
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 2);
    user_accounts->back =
        toolbar_button("go-previous", "_Back", G_CALLBACK(go_back), user_accounts);
    gtk_box_pack_start(GTK_BOX(bar), user_accounts->back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar),
                       toolbar_button("go-home", "_Home", G_CALLBACK(go_home), user_accounts),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    user_accounts->pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(user_accounts->pane), 12);
    gtk_widget_set_size_request(user_accounts->pane, 200, -1);
    GtkWidget *pane_bg = gtk_event_box_new();
    add_class(pane_bg, "xd-ua-pane");
    gtk_container_add(GTK_CONTAINER(pane_bg), user_accounts->pane);
    gtk_box_pack_start(GTK_BOX(body), pane_bg, FALSE, FALSE, 0);
    user_accounts->main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *main_bg = gtk_event_box_new();
    add_class(main_bg, "xd-ua-main");
    gtk_container_add(GTK_CONTAINER(main_bg), user_accounts->main);
    gtk_box_pack_start(GTK_BOX(body), main_bg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), body, TRUE, TRUE, 0);
}

int main(int argc, char **argv) {
    xd_gtk_app_preinit("org.xeondesktop.UserAccounts");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();
    static ua user_accounts;
    user_accounts.accounts = g_ptr_array_new_with_free_func(g_object_unref);
    user_accounts.history = g_array_new(FALSE, TRUE, sizeof(nav));
    GError *error = nullptr;
    user_accounts.manager =
        g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, ACCOUNTS,
                                      ACCOUNTS_PATH, ACCOUNTS, nullptr, &error);
    if (user_accounts.manager == nullptr) {
        g_printerr("xd-users: %s\n", error->message);
        g_error_free(error);
    } else {
        g_signal_connect(user_accounts.manager, "g-signal", G_CALLBACK(on_manager_signal),
                         &user_accounts);
    }
    load_accounts(&user_accounts);
    if (user_accounts.manager != nullptr && user_accounts.accounts->len == 0) {
        g_printerr("xd-users: no accounts from %s\n", ACCOUNTS);
        g_clear_object(&user_accounts.manager);
    }

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    /* --advanced: XP's `control userpasswords2` instead of the task pages. */
    for (int i = 1; i < argc; i++)
        user_accounts.advanced = user_accounts.advanced || strcmp(argv[i], "--advanced") == 0;
    user_accounts.cur = (nav){PAGE_HOME, nullptr};
    if (user_accounts.advanced) {
        build_advanced(&user_accounts);
    } else {
        build_window(&user_accounts);
        build_page(&user_accounts);
        gtk_widget_show_all(user_accounts.window);
    }
    g_signal_connect(user_accounts.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    gtk_main();
    clear_history(&user_accounts);
    g_array_free(user_accounts.history, TRUE);
    g_free(user_accounts.cur.user);
    g_ptr_array_free(user_accounts.accounts, TRUE);
    g_clear_object(&user_accounts.manager);
    g_free(user_accounts.my_path);
    g_free(user_accounts.picture);
    g_free(user_accounts.new_full);
    g_free(user_accounts.new_name);
    return EXIT_SUCCESS;
}
