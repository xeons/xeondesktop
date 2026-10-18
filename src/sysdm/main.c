/* SPDX-License-Identifier: MIT
 * xd-sysdm: the System Properties window and its General, Network
 * Identification and User Profiles tabs. Hardware and Advanced live in
 * hardware.c and advanced.c.
 *
 *   xd-sysdm [--page N]   0 General ... 4 Advanced
 *   xd-sysdm --devmgr     Device Manager on its own
 */
#include <ftw.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "../theme/draw.h"
#include "../users/accounts.h"
#include "sysdm.h"

/* ---- helpers ------------------------------------------------------------------- */

GtkWidget *sysdm_label(const char *text) {
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 30);
    return l;
}

GtkWidget *sysdm_group(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    /* A label (Device status) has margins rather than a border. */
    if (GTK_IS_CONTAINER(child)) {
        gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    } else {
        gtk_widget_set_margin_start(child, 8);
        gtk_widget_set_margin_end(child, 8);
        gtk_widget_set_margin_top(child, 8);
        gtk_widget_set_margin_bottom(child, 8);
    }
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

GtkWidget *sysdm_group_with_icon(const char *title, const char *icon, const char *text,
                                 GtkWidget *buttons) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), img, 0, 0, 1, 1);
    GtkWidget *l = sysdm_label(text);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 1, 0, 1, 1);
    if (buttons != nullptr)
        gtk_grid_attach(GTK_GRID(grid), buttons, 1, 1, 1, 1);
    return sysdm_group(title, grid);
}

GtkWidget *sysdm_button_row(GtkWidget *first, ...) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(box, GTK_ALIGN_END);
    va_list ap;
    va_start(ap, first);
    for (GtkWidget *b = first; b != nullptr; b = va_arg(ap, GtkWidget *))
        gtk_box_pack_start(GTK_BOX(box), b, FALSE, FALSE, 0);
    va_end(ap);
    return box;
}

GtkWidget *sysdm_button(const char *label, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    /* Windows' 75 (50 dialog units), wider only for a longer label. */
    gtk_widget_set_size_request(b, 75, -1);
    if (cb != nullptr)
        g_signal_connect(b, "clicked", cb, data);
    else
        gtk_widget_set_sensitive(b, FALSE);
    return b;
}

static void close_dialog(GtkDialog *d, [[maybe_unused]] int response,
                         [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(d));
}

GtkWidget *sysdm_dialog(sysdm *s, const char *title, const char *icon, gboolean cancel,
                        GtkWidget **dialog_out) {
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(s->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(d), icon);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    if (cancel)
        gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    if (dialog_out != nullptr)
        *dialog_out = d;
    else
        g_signal_connect(d, "response", G_CALLBACK(close_dialog), nullptr);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
    return box;
}

char *sysdm_grouped(guint64 v) {
    char digits[32];
    int n = g_snprintf(digits, sizeof(digits), "%" G_GUINT64_FORMAT, v);
    GString *out = g_string_new(nullptr);
    for (int i = 0; i < n; i++) {
        if (i > 0 && (n - i) % 3 == 0)
            g_string_append_c(out, ',');
        g_string_append_c(out, digits[i]);
    }
    return g_string_free(out, FALSE);
}

void sysdm_call_failed(GtkWindow *parent, const char *what, GError *err) {
    char *remote = g_dbus_error_get_remote_error(err);
    g_dbus_error_strip_remote_error(err);
    char *text = g_strdup_printf(
        "%s:\n\n%s%s", what, err->message,
        remote != nullptr && strstr(remote, "InteractiveAuthorizationRequired") != nullptr
            ? "\n\nChanging it needs an administrator's password, and no password prompt "
              "(polkit agent) is running."
            : "");
    xd_message_box(parent, "System Properties", XD_MB_ERROR, XD_MB_OK, FALSE, text);
    g_free(text);
    g_free(remote);
}

char *sysdm_read_line(const char *path) {
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        return nullptr;
    g_strchomp(text);
    return text;
}

/* /etc/os-release (or /usr/lib/os-release) as a key -> value table. */
static GHashTable *os_release(void) {
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *text = nullptr;
    if (!g_file_get_contents("/etc/os-release", &text, nullptr, nullptr) &&
        !g_file_get_contents("/usr/lib/os-release", &text, nullptr, nullptr))
        return t;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l != nullptr; l++) {
        char *eq = strchr(*l, '=');
        if (eq == nullptr || (*l)[0] == '#')
            continue;
        *eq = '\0';
        /* Values are shell-quoted. */
        char *value = g_shell_unquote(eq + 1, nullptr);
        if (value != nullptr)
            g_hash_table_replace(t, g_strdup(g_strstrip(*l)), value);
    }
    g_strfreev(lines);
    g_free(text);
    return t;
}

/* ---- General ------------------------------------------------------------------- */

static char *logo_name;

/* Windows 2000's beige monitor, with the distribution's logo on screen. */
static gboolean draw_monitor(GtkWidget *w, cairo_t *cr, [[maybe_unused]] gpointer data) {
    int width = gtk_widget_get_allocated_width(w);
    int bw = 140;
    int bh = 104;
    int x = (width - bw) / 2;
    int y = 4;
    /* Stand. */
    cairo_set_source_rgb(cr, 0xD4 / 255.0, 0xD0 / 255.0, 0xC8 / 255.0);
    cairo_rectangle(cr, x + bw / 2 - 18, y + bh, 36, 8);
    cairo_rectangle(cr, x + bw / 2 - 46, y + bh + 8, 92, 9);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_rectangle(cr, x + bw / 2 - 46, y + bh + 16, 92, 1);
    cairo_fill(cr);
    /* Body with a 3D edge. */
    cairo_set_source_rgb(cr, 0xD4 / 255.0, 0xD0 / 255.0, 0xC8 / 255.0);
    cairo_rectangle(cr, x, y, bw, bh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, x, y, bw, 1);
    cairo_rectangle(cr, x, y, 1, bh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_rectangle(cr, x, y + bh - 1, bw, 1);
    cairo_rectangle(cr, x + bw - 1, y, 1, bh);
    cairo_fill(cr);
    /* Screen: the Windows 2000 setup teal. */
    int sx = x + 10;
    int sy = y + 9;
    int sw = bw - 20;
    int sh = bh - 22;
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_rectangle(cr, sx - 1, sy - 1, sw + 2, sh + 2);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0, 0.5, 0.5);
    cairo_rectangle(cr, sx, sy, sw, sh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0.2, 0.75, 0.2);
    cairo_rectangle(cr, x + bw - 18, y + bh - 8, 4, 2);
    cairo_fill(cr);

    int logo = sh - 16;
    int lx = sx + (sw - logo) / 2;
    int ly = sy + (sh - logo) / 2;
    GtkIconTheme *icons = gtk_icon_theme_get_default();
    int scale = gtk_widget_get_scale_factor(w);
    GdkPixbuf *pb = nullptr;
    const char *names[] = {logo_name, "distributor-logo"};
    for (size_t i = 0; i < G_N_ELEMENTS(names) && pb == nullptr; i++) {
        if (names[i] != nullptr)
            pb = gtk_icon_theme_load_icon_for_scale(icons, names[i], logo, scale,
                                                    GTK_ICON_LOOKUP_FORCE_SIZE, nullptr);
    }
    if (pb != nullptr) {
        cairo_save(cr);
        cairo_translate(cr, lx, ly);
        cairo_scale(cr, 1.0 / scale, 1.0 / scale);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        g_object_unref(pb);
    } else {
        xd_draw_start_logo(cr, (xd_rect){lx, ly, logo, logo});
    }
    return FALSE;
}

static void add_section(GtkWidget *box, const char *title, const char *const *lines) {
    GtkWidget *t = sysdm_label(title);
    gtk_widget_set_margin_top(t, 10);
    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    for (const char *const *l = lines; *l != nullptr; l++) {
        if ((*l)[0] == '\0')
            continue;
        GtkWidget *v = sysdm_label(*l);
        gtk_label_set_selectable(GTK_LABEL(v), TRUE);
        gtk_widget_set_can_focus(v, FALSE);
        gtk_widget_set_margin_start(v, 20);
        gtk_box_pack_start(GTK_BOX(box), v, FALSE, FALSE, 0);
    }
}

static char *cpuinfo_value(const char *key) {
    char *text = nullptr;
    if (!g_file_get_contents("/proc/cpuinfo", &text, nullptr, nullptr))
        return nullptr;
    char *result = nullptr;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l != nullptr && result == nullptr; l++) {
        char *colon = strchr(*l, ':');
        if (colon == nullptr)
            continue;
        *colon = '\0';
        if (strcmp(g_strstrip(*l), key) == 0)
            result = g_strdup(g_strstrip(colon + 1));
    }
    g_strfreev(lines);
    g_free(text);
    return result;
}

static guint64 meminfo_kb(const char *key) {
    char *text = nullptr;
    if (!g_file_get_contents("/proc/meminfo", &text, nullptr, nullptr))
        return 0;
    guint64 kb = 0;
    char **lines = g_strsplit(text, "\n", -1);
    size_t klen = strlen(key);
    for (char **l = lines; *l != nullptr; l++) {
        if (strncmp(*l, key, klen) == 0 && (*l)[klen] == ':')
            kb = g_ascii_strtoull(*l + klen + 1, nullptr, 10);
    }
    g_strfreev(lines);
    g_free(text);
    return kb;
}

/* The machine ID shown like a Windows product ID: 12345-678-9012345-67890. */
static char *product_id(void) {
    char *id = sysdm_read_line("/etc/machine-id");
    if (id == nullptr || strlen(id) < 20) {
        g_free(id);
        return nullptr;
    }
    char *out = g_strdup_printf("%.5s-%.3s-%.7s-%.5s", id, id + 5, id + 8, id + 15);
    g_free(id);
    return out;
}

static GtkWidget *build_general(void) {
    GHashTable *os = os_release();
    logo_name = g_strdup(g_hash_table_lookup(os, "LOGO"));

    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *monitor = gtk_drawing_area_new();
    gtk_widget_set_size_request(monitor, 170, 130);
    gtk_widget_set_valign(monitor, GTK_ALIGN_START);
    gtk_widget_set_margin_top(monitor, 40);
    g_signal_connect(monitor, "draw", G_CALLBACK(draw_monitor), nullptr);
    gtk_box_pack_start(GTK_BOX(page), monitor, FALSE, FALSE, 0);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(page), text, TRUE, TRUE, 0);

    struct utsname u;
    uname(&u);
    const char *name = g_hash_table_lookup(os, "PRETTY_NAME");
    if (name == nullptr)
        name = g_hash_table_lookup(os, "NAME");
    const char *version = g_hash_table_lookup(os, "VERSION");
    if (version == nullptr)
        version = g_hash_table_lookup(os, "VERSION_ID");
    if (version == nullptr)
        version = g_hash_table_lookup(os, "BUILD_ID");
    char *ver = version != nullptr ? g_strdup_printf("Version %s", version) : g_strdup("");
    char *kernel = g_strdup_printf("Linux %s (%s)", u.release, u.machine);
    char *desktop = g_strdup_printf("XeonDesktop %s", XD_VERSION);
    const char *system[] = {name != nullptr ? name : "Linux", ver, kernel, desktop, nullptr};
    add_section(text, "System:", system);

    struct passwd *pw = getpwuid(getuid());
    char full[256] = "";
    if (pw != nullptr && pw->pw_gecos != nullptr) {
        g_strlcpy(full, pw->pw_gecos, sizeof(full));
        char *comma = strchr(full, ',');
        if (comma != nullptr)
            *comma = '\0';
    }
    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);
    char *account = g_strdup_printf("%s@%s", pw != nullptr ? pw->pw_name : "user", host);
    char *pid = product_id();
    const char *registered[] = {full, account, pid != nullptr ? pid : "", nullptr};
    add_section(text, "Registered to:", registered);

    char *cpu = cpuinfo_value("model name");
    if (cpu == nullptr)
        cpu = cpuinfo_value("Hardware");
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    char *max_khz = sysdm_read_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    char *speed = nullptr;
    if (max_khz != nullptr)
        speed = g_strdup_printf("%ld processor%s, %.2f GHz", cpus, cpus == 1 ? "" : "s",
                                g_ascii_strtod(max_khz, nullptr) / 1e6);
    else
        speed = g_strdup_printf("%ld processor%s", cpus, cpus == 1 ? "" : "s");
    char *vendor = sysdm_read_line("/sys/class/dmi/id/sys_vendor");
    char *product = sysdm_read_line("/sys/class/dmi/id/product_name");
    gboolean efi = g_file_test("/sys/firmware/efi", G_FILE_TEST_IS_DIR);
    char *machine = g_strdup_printf("%s%s%s%s%s", vendor != nullptr ? vendor : "",
                                    vendor != nullptr && product != nullptr ? " " : "",
                                    product != nullptr ? product : "",
                                    vendor != nullptr || product != nullptr ? ", " : "",
                                    efi ? "UEFI" : "BIOS");
    char *kb = sysdm_grouped(meminfo_kb("MemTotal"));
    char *ram = g_strdup_printf("%s KB RAM", kb);
    const char *computer[] = {cpu != nullptr ? cpu : "", speed, machine, ram, nullptr};
    add_section(text, "Computer:", computer);

    g_free(ver);
    g_free(kernel);
    g_free(desktop);
    g_free(account);
    g_free(pid);
    g_free(cpu);
    g_free(max_khz);
    g_free(speed);
    g_free(vendor);
    g_free(product);
    g_free(machine);
    g_free(kb);
    g_free(ram);
    g_hash_table_unref(os);
    return page;
}

/* ---- Network Identification ------------------------------------------------------ */

static char *hostname_prop(sysdm *s, const char *prop) {
    if (s->hostname1 == nullptr)
        return nullptr;
    GVariant *v = g_dbus_proxy_get_cached_property(s->hostname1, prop);
    if (v == nullptr)
        return nullptr;
    char *out = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_dup_string(v, nullptr)
                                                               : nullptr;
    g_variant_unref(v);
    if (out != nullptr && out[0] == '\0')
        g_clear_pointer(&out, g_free);
    return out;
}

/* Samba's workgroup, the closest thing Linux has to Windows' Workgroup. */
static char *samba_workgroup(void) {
    GKeyFile *kf = g_key_file_new();
    char *wg = nullptr;
    if (g_key_file_load_from_file(kf, "/etc/samba/smb.conf", G_KEY_FILE_NONE, nullptr))
        wg = g_key_file_get_string(kf, "global", "workgroup", nullptr);
    g_key_file_free(kf);
    if (wg != nullptr)
        g_strstrip(wg);
    return wg;
}

static void refresh_identity(sysdm *s) {
    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);
    /* The DNS domain comes from the host name if it has one. */
    const char *dot = strchr(host, '.');
    char *domain = dot != nullptr ? g_strdup(dot + 1) : nullptr;
    gtk_label_set_text(GTK_LABEL(s->full_name), host);
    if (domain != nullptr) {
        gtk_label_set_text(GTK_LABEL(s->domain), domain);
    } else {
        char *wg = samba_workgroup();
        char *text = wg != nullptr ? g_strdup_printf("Workgroup %s (Samba)", wg)
                                   : g_strdup("(none)");
        gtk_label_set_text(GTK_LABEL(s->domain), text);
        g_free(text);
        g_free(wg);
    }
    static const char *const chassis_names[][2] = {
        {"desktop", "Desktop"},   {"laptop", "Laptop"},       {"convertible", "Convertible"},
        {"server", "Server"},     {"tablet", "Tablet"},       {"handset", "Phone"},
        {"watch", "Watch"},       {"embedded", "Embedded"},   {"vm", "Virtual machine"},
        {"container", "Container"},
    };
    char *chassis = hostname_prop(s, "Chassis");
    for (size_t i = 0; chassis != nullptr && i < G_N_ELEMENTS(chassis_names); i++) {
        if (strcmp(chassis, chassis_names[i][0]) == 0) {
            g_free(chassis);
            chassis = g_strdup(chassis_names[i][1]);
            break;
        }
    }
    char *vendor = hostname_prop(s, "HardwareVendor");
    char *model = hostname_prop(s, "HardwareModel");
    char *line = g_strdup_printf("%s%s%s%s%s", chassis != nullptr ? chassis : "unknown",
                                 vendor != nullptr || model != nullptr ? " — " : "",
                                 vendor != nullptr ? vendor : "",
                                 vendor != nullptr && model != nullptr ? " " : "",
                                 model != nullptr ? model : "");
    gtk_label_set_text(GTK_LABEL(s->chassis), line);
    g_free(line);
    g_free(chassis);
    g_free(vendor);
    g_free(model);
    g_free(domain);
}

/* What to do once a hostname1 call is done; `ok` if it succeeded. */
typedef void (*hostname_done)(sysdm *s, gboolean ok, gpointer data);

typedef struct hostname_req {
    sysdm *s;
    hostname_done done;
    gpointer data;
} hostname_req;

static void hostname_call_done(GObject *src, GAsyncResult *res, gpointer data) {
    hostname_req *req = data;
    GError *err = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (r == nullptr && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err); /* the window is gone */
        g_free(req);
        return;
    }
    sysdm *s = req->s;
    if (r == nullptr) {
        sysdm_call_failed(GTK_WINDOW(s->window), "The computer's name could not be changed", err);
        g_error_free(err);
    } else {
        g_variant_unref(r);
    }
    req->done(s, r != nullptr, req->data);
    g_free(req);
}

/* Calls a hostname1 method without waiting for it: polkit's password
 * prompt may be up for a while. Errors (no polkit agent, not allowed) are
 * shown; then `done` runs. */
static void hostname_call(sysdm *s, const char *method, const char *value, hostname_done done,
                          gpointer data) {
    if (s->hostname1 == nullptr) {
        xd_message_box(GTK_WINDOW(s->window), "System Properties", XD_MB_ERROR, XD_MB_OK, FALSE,
                       "The host name service (systemd-hostnamed) isn't available.");
        done(s, FALSE, data);
        return;
    }
    hostname_req *req = g_new0(hostname_req, 1);
    req->s = s;
    req->done = done;
    req->data = data;
    g_dbus_proxy_call(s->hostname1, method, g_variant_new("(sb)", value, TRUE),
                      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 120000, s->calls,
                      hostname_call_done, req);
}

static gboolean valid_hostname(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > 63 || name[0] == '-' || name[len - 1] == '-')
        return FALSE;
    for (const char *p = name; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '-')
            return FALSE;
    }
    return TRUE;
}

typedef struct rename_dlg {
    sysdm *s;
    GtkWidget *dialog;
    GtkWidget *entry;
    GtkWidget *full;
    gboolean busy; /* renaming */
} rename_dlg;

static void on_rename_changed(GtkEditable *e, gpointer data) {
    rename_dlg *r = data;
    gtk_label_set_text(GTK_LABEL(r->full), gtk_entry_get_text(GTK_ENTRY(e)));
}

static void renamed(sysdm *s, gboolean ok, gpointer data) {
    rename_dlg *r = data;
    r->busy = FALSE;
    gtk_widget_set_sensitive(r->dialog, TRUE);
    if (!ok)
        return;
    refresh_identity(s);
    gtk_widget_destroy(r->dialog);
    g_free(r);
}

static void on_rename_response(GtkDialog *d, int response, gpointer data) {
    rename_dlg *r = data;
    if (r->busy)
        return; /* closes once the name is set */
    if (response == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(r->entry));
        if (!valid_hostname(name)) {
            xd_message_box(GTK_WINDOW(d), "Identification Changes", XD_MB_WARNING, XD_MB_OK,
                           FALSE,
                           "The computer name may contain only letters (A-Z), numbers (0-9) and "
                           "hyphens (-), up to 63 characters, and can't begin or end with a "
                           "hyphen.");
            return;
        }
        r->busy = TRUE;
        gtk_widget_set_sensitive(r->dialog, FALSE);
        hostname_call(r->s, "SetStaticHostname", name, renamed, r);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(d));
    g_free(r);
}

static void on_rename(GtkButton *b, gpointer data) {
    (void)b;
    sysdm *s = data;
    GtkWidget *d = nullptr;
    GtkWidget *box = sysdm_dialog(s, "Identification Changes", "computer", TRUE, &d);
    gtk_box_pack_start(GTK_BOX(box),
                       sysdm_label("You can change the name of this computer. Programs and other "
                                   "computers on the network see the new name right away."),
                       FALSE, FALSE, 0);
    rename_dlg *r = g_new0(rename_dlg, 1);
    r->s = s;
    r->dialog = d;
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Computer name:"), 0, 0, 1, 1);
    r->entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(r->entry), 63);
    gtk_entry_set_activates_default(GTK_ENTRY(r->entry), TRUE);
    char host[256] = "localhost";
    gethostname(host, sizeof(host) - 1);
    gtk_entry_set_text(GTK_ENTRY(r->entry), host);
    gtk_widget_set_hexpand(r->entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), r->entry, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Full computer name:"), 0, 1, 1, 1);
    r->full = sysdm_label(host);
    gtk_grid_attach(GTK_GRID(grid), r->full, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    g_signal_connect(r->entry, "changed", G_CALLBACK(on_rename_changed), r);
    g_signal_connect(d, "response", G_CALLBACK(on_rename_response), r);
    gtk_widget_set_size_request(d, 360, -1);
    gtk_widget_show_all(d);
}

static void on_description_changed([[maybe_unused]] GtkEditable *e, gpointer data) {
    sysdm *s = data;
    s->description_dirty = TRUE;
    gtk_widget_set_sensitive(s->apply, TRUE);
}

static GtkWidget *build_network(sysdm *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("computer", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top),
                       sysdm_label("XeonDesktop uses the following information to identify your "
                                   "computer on the network."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_top(grid, 8);
    int row = 0;
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Computer description:"), 0, row, 1, 1);
    s->description = gtk_entry_new();
    gtk_widget_set_hexpand(s->description, TRUE);
    char *pretty = hostname_prop(s, "PrettyHostname");
    if (pretty != nullptr)
        gtk_entry_set_text(GTK_ENTRY(s->description), pretty);
    g_free(pretty);
    gtk_grid_attach(GTK_GRID(grid), s->description, 1, row++, 1, 1);
    GtkWidget *example = sysdm_label("For example: \"Kitchen Computer\" or \"Mary's Computer\".");
    gtk_style_context_add_class(gtk_widget_get_style_context(example), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), example, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Full computer name:"), 0, row, 1, 1);
    s->full_name = sysdm_label("");
    gtk_label_set_selectable(GTK_LABEL(s->full_name), TRUE);
    gtk_grid_attach(GTK_GRID(grid), s->full_name, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Domain:"), 0, row, 1, 1);
    s->domain = sysdm_label("");
    gtk_grid_attach(GTK_GRID(grid), s->domain, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Computer type:"), 0, row, 1, 1);
    s->chassis = sysdm_label("");
    gtk_grid_attach(GTK_GRID(grid), s->chassis, 1, row++, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), grid, FALSE, FALSE, 0);
    g_signal_connect(s->description, "changed", G_CALLBACK(on_description_changed), s);

    GtkWidget *rename = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(rename), 12);
    gtk_widget_set_margin_top(rename, 24);
    GtkWidget *hint = sysdm_label("To rename this computer, click Properties.");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_grid_attach(GTK_GRID(rename), hint, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(rename), sysdm_button("_Properties", G_CALLBACK(on_rename), s), 1, 0,
                    1, 1);
    gtk_box_pack_start(GTK_BOX(page), rename, FALSE, FALSE, 0);
    refresh_identity(s);
    return page;
}

/* ---- User Profiles --------------------------------------------------------------- */

enum { COL_NAME, COL_SIZE, COL_TYPE, COL_MODIFIED, COL_HOME, COL_UID, NCOLS };

static _Thread_local guint64 walk_total;

static int add_size(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)path;
    (void)ftw;
    if (flag == FTW_F)
        walk_total += (guint64)st->st_blocks * 512;
    return 0;
}

static void size_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                        [[maybe_unused]] GCancellable *c) {
    const char *home = data;
    walk_total = 0;
    /* Same file system only, without following links. */
    if (nftw(home, add_size, 32, FTW_PHYS | FTW_MOUNT) != 0 && walk_total == 0) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "unreadable");
        return;
    }
    g_task_return_pointer(task, g_memdup2(&walk_total, sizeof(walk_total)), g_free);
}

typedef struct size_req {
    sysdm *s;
    GtkTreeRowReference *row;
} size_req;

static void on_size_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    size_req *req = data;
    GError *err = nullptr;
    guint64 *total = g_task_propagate_pointer(G_TASK(res), &err);
    gboolean cancelled = err != nullptr && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (!cancelled && gtk_tree_row_reference_valid(req->row)) {
        GtkTreePath *path = gtk_tree_row_reference_get_path(req->row);
        GtkTreeIter it;
        GtkTreeModel *m = gtk_tree_row_reference_get_model(req->row);
        if (gtk_tree_model_get_iter(m, &it, path)) {
            char *text = nullptr;
            if (total == nullptr) {
                text = g_strdup("Unknown");
            } else {
                double mb = (double)*total / (1024.0 * 1024.0);
                text = mb >= 1024 ? g_strdup_printf("%.1f GB", mb / 1024)
                                  : g_strdup_printf("%.1f MB", mb);
            }
            gtk_list_store_set(GTK_LIST_STORE(m), &it, COL_SIZE, text, -1);
            g_free(text);
        }
        gtk_tree_path_free(path);
    }
    g_free(total);
    g_clear_error(&err);
    gtk_tree_row_reference_free(req->row);
    g_free(req);
}

static void fill_profiles(sysdm *s) {
    /* Regular accounts: UID_MIN..UID_MAX as login.defs sets them. */
    guint64 uid_min = 1000;
    guint64 uid_max = 60000;
    char *defs = nullptr;
    if (g_file_get_contents("/etc/login.defs", &defs, nullptr, nullptr)) {
        char **lines = g_strsplit(defs, "\n", -1);
        for (char **l = lines; *l != nullptr; l++) {
            char *line = g_strstrip(*l);
            if (g_str_has_prefix(line, "UID_MIN"))
                uid_min = g_ascii_strtoull(line + 7, nullptr, 10);
            else if (g_str_has_prefix(line, "UID_MAX"))
                uid_max = g_ascii_strtoull(line + 7, nullptr, 10);
        }
        g_strfreev(lines);
        g_free(defs);
    }
    /* XD_SYSDM_PASSWD: a passwd file to list instead, for the UI tests,
     * which can't add users to the system's. */
    const char *file = g_getenv("XD_SYSDM_PASSWD");
    FILE *f = file != nullptr ? fopen(file, "re") : nullptr;
    if (f == nullptr)
        setpwent();
    for (struct passwd *pw = f != nullptr ? fgetpwent(f) : getpwent(); pw != nullptr;
         pw = f != nullptr ? fgetpwent(f) : getpwent()) {
        if (pw->pw_uid < uid_min || pw->pw_uid > uid_max || pw->pw_dir == nullptr)
            continue;
        struct stat st;
        if (stat(pw->pw_dir, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        GDateTime *dt = g_date_time_new_from_unix_local(st.st_mtime);
        char *modified = g_date_time_format(dt, "%x");
        g_date_time_unref(dt);
        GtkTreeIter it;
        gtk_list_store_append(s->profiles, &it);
        gtk_list_store_set(s->profiles, &it, COL_NAME, pw->pw_name, COL_SIZE, "Calculating...",
                           COL_TYPE, "Local", COL_MODIFIED, modified, COL_HOME, pw->pw_dir,
                           COL_UID, (guint)pw->pw_uid, -1);
        g_free(modified);

        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(s->profiles), &it);
        size_req *req = g_new0(size_req, 1);
        req->s = s;
        req->row = gtk_tree_row_reference_new(GTK_TREE_MODEL(s->profiles), path);
        gtk_tree_path_free(path);
        GTask *task = g_task_new(nullptr, s->sizing, on_size_done, req);
        g_task_set_task_data(task, g_strdup(pw->pw_dir), g_free);
        g_task_run_in_thread(task, size_thread);
        g_object_unref(task);
    }
    if (f != nullptr)
        fclose(f);
    else
        endpwent();
}

static void on_open_profile([[maybe_unused]] GtkButton *b, gpointer data) {
    GtkTreeView *view = data;
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(view), &m, &it))
        return;
    char *home = nullptr;
    gtk_tree_model_get(m, &it, COL_HOME, &home, -1);
    char *uri = g_filename_to_uri(home, nullptr, nullptr);
    if (uri != nullptr)
        g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
    g_free(uri);
    g_free(home);
}

/* Delete is for another user's profile: XP refused the one in use. */
static void on_profile_selected(GtkTreeSelection *sel, gpointer data) {
    sysdm *s = data;
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    guint uid = 0;
    gboolean any = gtk_tree_selection_get_selected(sel, &m, &it);
    if (any)
        gtk_tree_model_get(m, &it, COL_UID, &uid, -1);
    gtk_widget_set_sensitive(s->profile_delete, any && uid != (guint)getuid());
}

typedef struct profile_req {
    sysdm *s;
    GtkTreeRowReference *row;
} profile_req;

static void on_profile_deleted(GObject *src, GAsyncResult *res, gpointer data) {
    profile_req *req = data;
    GError *err = nullptr;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r == nullptr && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err); /* the window is gone */
    } else if (r == nullptr) {
        gtk_widget_set_sensitive(req->s->notebook, TRUE);
        sysdm_call_failed(GTK_WINDOW(req->s->window), "The profile could not be deleted", err);
        g_error_free(err);
    } else {
        gtk_widget_set_sensitive(req->s->notebook, TRUE);
        GtkTreePath *path = gtk_tree_row_reference_get_path(req->row);
        GtkTreeIter it;
        if (path != nullptr &&
            gtk_tree_model_get_iter(GTK_TREE_MODEL(req->s->profiles), &it, path))
            gtk_list_store_remove(req->s->profiles, &it);
        gtk_tree_path_free(path);
        g_variant_unref(r);
    }
    gtk_tree_row_reference_free(req->row);
    g_free(req);
}

/* Deletes the selected profile. An account without its home folder would
 * be broken, so AccountsService deletes both, as User Accounts' Delete
 * Files does; it asks polkit for an administrator's password. */
static void on_delete_profile([[maybe_unused]] GtkButton *b, gpointer data) {
    sysdm *s = data;
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(s->profiles_view)), &m, &it))
        return;
    char *name = nullptr;
    char *home = nullptr;
    guint uid = 0;
    gtk_tree_model_get(m, &it, COL_NAME, &name, COL_HOME, &home, COL_UID, &uid, -1);
    if (uid == (guint)getuid()) {
        /* Greyed already; this is in case it's reached anyway. */
        xd_message_box(GTK_WINDOW(s->window), "System Properties", XD_MB_WARNING, XD_MB_OK,
                       FALSE, "The profile you are logged on with can't be deleted.");
    } else {
        char *text = g_strdup_printf("Are you sure you want to delete this profile?\n\nOn Linux "
                                     "a profile can't go without its account, so %s's account is "
                                     "deleted too, with its home folder %s.",
                                     name, home);
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
        if (xd_message_box(GTK_WINDOW(s->window), "Confirm Delete", XD_MB_QUESTION, XD_MB_YES_NO,
                           TRUE, text) != GTK_RESPONSE_YES) {
            /* Not deleted. */
        } else if (bus == nullptr) {
            xd_message_box(GTK_WINDOW(s->window), "System Properties", XD_MB_ERROR, XD_MB_OK,
                           FALSE, "The profile could not be deleted: the system bus isn't "
                                  "available.");
        } else {
            profile_req *req = g_new0(profile_req, 1);
            req->s = s;
            GtkTreePath *path = gtk_tree_model_get_path(m, &it);
            req->row = gtk_tree_row_reference_new(m, path);
            gtk_tree_path_free(path);
            /* Greyed until AccountsService answers. */
            gtk_widget_set_sensitive(s->notebook, FALSE);
            xd_accounts_delete_user(bus, (gint64)uid, true, s->calls, on_profile_deleted, req);
        }
        if (bus != nullptr)
            g_object_unref(bus);
        g_free(text);
    }
    g_free(name);
    g_free(home);
}

static GtkWidget *build_profiles(sysdm *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("system-users", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top),
                       sysdm_label("User profiles contain desktop settings and other information "
                                   "related to your logon. On Linux, each user's profile is "
                                   "their home folder."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), sysdm_label("Profiles stored on this computer:"), FALSE,
                       FALSE, 4);

    s->profiles = gtk_list_store_new(NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(s->profiles));
    s->profiles_view = view;
    g_object_unref(s->profiles);
    static const char *const titles[] = {"Name", "Size", "Type", "Modified"};
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(titles[i], r, "text", i,
                                                                        nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        if (i == 0)
            gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
    }
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), COL_HOME);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 170);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(page), scroll, TRUE, TRUE, 0);

    /* Open shows the folder; Delete goes through AccountsService. Linux
     * has no roaming profiles to change the type to, and nothing that
     * copies one user's settings to another (cp -a, then fixing owners
     * and paths, is an administrator's job). */
    GtkWidget *type = sysdm_button("Change _Type...", nullptr, nullptr);
    gtk_widget_set_tooltip_text(type, "Linux profiles are always local: there are no roaming "
                                      "profiles to change to.");
    s->profile_delete = sysdm_button("_Delete", G_CALLBACK(on_delete_profile), s);
    GtkWidget *copy = sysdm_button("Copy _To...", nullptr, nullptr);
    gtk_widget_set_tooltip_text(copy, "Linux has no way to copy one user's settings to another "
                                      "user; an administrator copies the files and changes their "
                                      "owner.");
    gtk_box_pack_start(GTK_BOX(page),
                       sysdm_button_row(sysdm_button("_Open", G_CALLBACK(on_open_profile), view),
                                        type, s->profile_delete, copy, nullptr),
                       FALSE, FALSE, 0);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    g_signal_connect(sel, "changed", G_CALLBACK(on_profile_selected), s);
    on_profile_selected(sel, s);
    s->sizing = g_cancellable_new();
    fill_profiles(s);
    return page;
}

/* ---- window --------------------------------------------------------------------- */

static void applied(sysdm *s, gboolean ok, gpointer data) {
    gboolean close = GPOINTER_TO_INT(data);
    s->applying = FALSE;
    if (ok)
        s->description_dirty = FALSE;
    gtk_widget_set_sensitive(s->apply, s->description_dirty);
    if (ok && close)
        gtk_widget_destroy(s->window);
}

/* Sets the description, if it changed; OK (`close`) closes the window
 * once it's set. False when there's nothing to set. */
static gboolean apply(sysdm *s, gboolean close) {
    if (!s->description_dirty)
        return FALSE;
    s->applying = TRUE;
    gtk_widget_set_sensitive(s->apply, FALSE);
    hostname_call(s, "SetPrettyHostname", gtk_entry_get_text(GTK_ENTRY(s->description)), applied,
                  GINT_TO_POINTER(close));
    return TRUE;
}

static void on_response(GtkDialog *d, int response, gpointer data) {
    sysdm *s = data;
    if (s->applying && (response == GTK_RESPONSE_APPLY || response == GTK_RESPONSE_OK))
        return; /* still setting it */
    if (response == GTK_RESPONSE_APPLY) {
        apply(s, FALSE);
        return;
    }
    if (response == GTK_RESPONSE_OK && apply(s, TRUE))
        return;
    gtk_widget_destroy(GTK_WIDGET(d));
}

static void on_destroy([[maybe_unused]] GtkWidget *w, gpointer data) {
    sysdm *s = data;
    /* The list's selection changes as it goes; Delete may be gone first. */
    g_signal_handlers_disconnect_by_data(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(s->profiles_view)), s);
    g_cancellable_cancel(s->sizing);
    g_clear_object(&s->sizing);
    g_cancellable_cancel(s->calls);
    g_clear_object(&s->calls);
    g_clear_object(&s->hostname1);
    g_free(s);
}

static int start_page;
static gboolean devmgr_only;

static void open_window(GtkApplication *app) {
    if (devmgr_only) {
        /* Device Manager on its own, as devmgmt.msc. */
        static sysdm alone;
        alone.app = app;
        sysdm_device_manager(&alone);
        return;
    }
    sysdm *s = g_new0(sysdm, 1);
    s->app = app;
    s->calls = g_cancellable_new();
    s->hostname1 = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.freedesktop.hostname1",
        "/org/freedesktop/hostname1", "org.freedesktop.hostname1", nullptr, nullptr);

    s->window = gtk_dialog_new();
    gtk_window_set_application(GTK_WINDOW(s->window), app);
    gtk_window_set_title(GTK_WINDOW(s->window), "System Properties");
    gtk_window_set_icon_name(GTK_WINDOW(s->window), "computer");
    gtk_window_set_resizable(GTK_WINDOW(s->window), FALSE);
    g_signal_connect(s->window, "destroy", G_CALLBACK(on_destroy), s);

    s->notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(s->notebook), 6);
    gtk_widget_set_size_request(s->notebook, 400, 400);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(s->window))),
                      s->notebook);
    xd_gtk_notebook_ctrl_tab(s->window, GTK_NOTEBOOK(s->notebook));
    GtkWidget *ok = gtk_dialog_add_button(GTK_DIALOG(s->window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(s->window), "Cancel", GTK_RESPONSE_CANCEL);
    s->apply = gtk_dialog_add_button(GTK_DIALOG(s->window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(s->window), GTK_RESPONSE_OK);
    g_signal_connect(s->window, "response", G_CALLBACK(on_response), s);

    struct {
        const char *title;
        GtkWidget *page;
    } pages[] = {
        {"General", build_general()},
        {"Network Identification", build_network(s)},
        {"Hardware", sysdm_hardware_page(s)},
        {"User Profiles", build_profiles(s)},
        {"Advanced", sysdm_advanced_page(s)},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(pages); i++)
        gtk_notebook_append_page(GTK_NOTEBOOK(s->notebook), pages[i].page,
                                 gtk_label_new(pages[i].title));
    gtk_widget_set_sensitive(s->apply, FALSE);
    gtk_widget_show_all(s->window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), start_page);
    gtk_widget_grab_focus(ok);
}

static void on_activate(GtkApplication *app, [[maybe_unused]] gpointer data) {
    GList *windows = gtk_application_get_windows(app);
    if (windows != nullptr) {
        gtk_window_present(windows->data);
        return;
    }
    open_window(app);
}

static void on_startup([[maybe_unused]] GApplication *app, [[maybe_unused]] gpointer data) {
    xd_gtk_app_follow_theme();
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            int page = atoi(argv[++i]); /* CLAMP evaluates its argument more than once */
            start_page = CLAMP(page, 0, 4);
        }
        else if (strcmp(argv[i], "--devmgr") == 0)
            devmgr_only = TRUE;
    }
    xd_gtk_app_preinit("org.xeondesktop.SystemProperties");
    GtkApplication *app = gtk_application_new("org.xeondesktop.SystemProperties",
                                              G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(on_startup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), nullptr);
    int rc = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    g_free(logo_name);
    return rc;
}
