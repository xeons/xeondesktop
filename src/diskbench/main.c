/* SPDX-License-Identifier: MIT
 * xd-diskbench: Disk Benchmark and Health (Administrative Tools), a
 * Linux-only tool with no Windows original, laid out as XP's tools are.
 *
 * Benchmark measures a drive's (or folder's) speed with a test file of
 * its own: sequential and random reads and writes at the block sizes,
 * queue depths and thread counts of the profile, the best of several
 * runs. Health shows each drive's S.M.A.R.T. data from UDisks2.
 *
 *   xd-diskbench [--page N] [--drive MOUNTPOINT] [--folder DIR] [--disk DEVICE]
 */
#include <gio/gunixmounts.h>
#include <stdlib.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "benchlib.h"
#include "engine.h"
#include "health.h"

#define TITLE "Disk Benchmark and Health"

static constexpr unsigned MAX_RUNS = 9;
static constexpr unsigned DEFAULT_RUNS = 5;
static constexpr unsigned DEFAULT_MEASURE_SECONDS = 5;
static constexpr unsigned DEFAULT_INTERVAL_SECONDS = 5;
static constexpr unsigned MAX_SECONDS = 60;

/* A place a test file can go: a mounted file system's root (or the
 * user's home on it, when the root isn't writable), or a folder. */
typedef struct drive_entry {
    char *label;     /* "Filesystem root (/), 12.3 GB free" */
    char *name;      /* "Filesystem root (/)" */
    char *mount_point;
    char *directory; /* where the hidden folder for the test file goes */
    char *filesystem;
} drive_entry;

/* What the Settings dialog sets, and the main window's choices. */
typedef struct bench_settings {
    xd_bench_profile profile;
    size_t test_count;
    xd_bench_test tests[XD_BENCH_MAX_ROWS];
    unsigned measure_seconds, interval_seconds;
    bool random_data;
    unsigned runs;
    uint64_t test_size;
    xd_bench_unit unit;
} bench_settings;

typedef struct result_row {
    GtkWidget *button, *button_stack, *title, *subtitle;
    GtkWidget *read_frame, *read_label, *write_frame, *write_label;
    xd_bench_measure read_result, write_result;
} result_row;

typedef struct diskbench {
    GtkWidget *window, *notebook, *status;
    GtkWidget *drive_combo, *folder_button, *runs_combo, *size_combo, *unit_combo;
    GtkWidget *settings_button, *all_button, *all_stack, *results_grid, *read_header,
        *write_header, *cached_label;
    GPtrArray *drives; /* drive_entry * */
    bench_settings settings;
    result_row rows[XD_BENCH_MAX_ROWS];
    health_tab *health;
    /* The run under way, if any. */
    xd_bench_engine *engine;
    xd_bench_plan plan;
    int running_row; /* -1: All */
    bool cached, quit_when_finished;
    /* What the results were measured on, for Copy. */
    char *result_drive, *result_filesystem, *result_date;
    bench_settings result_settings;
    bool result_cached;
} diskbench;

/* ---- settings ---------------------------------------------------------------------- */

static void set_default_settings(bench_settings *settings) {
    *settings = (bench_settings){
        .profile = XD_BENCH_PROFILE_DEFAULT,
        .measure_seconds = DEFAULT_MEASURE_SECONDS,
        .interval_seconds = DEFAULT_INTERVAL_SECONDS,
        .random_data = true,
        .runs = DEFAULT_RUNS,
        .test_size = XD_BENCH_DEFAULT_TEST_SIZE,
        .unit = XD_BENCH_UNIT_MEGABYTES,
    };
    settings->test_count = xd_bench_get_profile_tests(settings->profile, settings->tests);
}

static char *get_settings_path(void) {
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "diskbench.ini", nullptr);
}

static const char *const profile_keys[] = {"default", "peak", "realistic"};
static_assert(XD_ARRAY_LEN(profile_keys) == XD_BENCH_PROFILE_COUNT);

static unsigned get_clamped(GKeyFile *file, const char *key, unsigned min, unsigned max,
                            unsigned fallback) {
    GError *error = nullptr;
    int value = g_key_file_get_integer(file, "Benchmark", key, &error);
    if (error != nullptr) {
        g_error_free(error);
        return fallback;
    }
    return (unsigned)XD_CLAMP(value, (int)min, (int)max);
}

static void load_settings(bench_settings *settings) {
    set_default_settings(settings);
    g_autofree char *path = get_settings_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, nullptr))
        return;
    g_autofree char *profile = g_key_file_get_string(file, "Benchmark", "Profile", nullptr);
    for (size_t i = 0; profile != nullptr && i < XD_ARRAY_LEN(profile_keys); i++)
        if (strcmp(profile, profile_keys[i]) == 0)
            settings->profile = (xd_bench_profile)i;
    settings->test_count = xd_bench_get_profile_tests(settings->profile, settings->tests);
    for (size_t i = 0; i < settings->test_count; i++) {
        g_autofree char *key = g_strdup_printf("Test%zu", i + 1);
        gsize length = 0;
        g_autofree int *values =
            g_key_file_get_integer_list(file, "Benchmark", key, &length, nullptr);
        if (values == nullptr || length != 3 || values[0] <= 0 || values[1] <= 0 ||
            values[2] <= 0)
            continue;
        xd_bench_test test = settings->tests[i];
        test.block_size = (uint32_t)values[0];
        test.queue_depth = (uint32_t)values[1];
        test.threads = (uint32_t)values[2];
        if (xd_bench_is_test_valid(&test))
            settings->tests[i] = test;
    }
    settings->measure_seconds =
        get_clamped(file, "MeasureSeconds", 1, MAX_SECONDS, DEFAULT_MEASURE_SECONDS);
    settings->interval_seconds =
        get_clamped(file, "IntervalSeconds", 0, MAX_SECONDS, DEFAULT_INTERVAL_SECONDS);
    settings->runs = get_clamped(file, "Runs", 1, MAX_RUNS, DEFAULT_RUNS);
    unsigned unit = get_clamped(file, "Unit", 0, XD_BENCH_UNIT_COUNT - 1, 0);
    settings->unit = (xd_bench_unit)unit;
    g_autofree char *data = g_key_file_get_string(file, "Benchmark", "TestData", nullptr);
    settings->random_data = data == nullptr || strcmp(data, "zeros") != 0;
    g_autofree char *size = g_key_file_get_string(file, "Benchmark", "TestSize", nullptr);
    guint64 bytes = size != nullptr ? g_ascii_strtoull(size, nullptr, 10) : 0;
    for (size_t i = 0; i < xd_bench_test_size_count; i++)
        if (xd_bench_test_sizes[i] == bytes)
            settings->test_size = bytes;
}

static void save_settings(diskbench *bench) {
    const bench_settings *settings = &bench->settings;
    g_autofree char *path = get_settings_path();
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_key_file_set_string(file, "Benchmark", "Profile", profile_keys[settings->profile]);
    for (size_t i = 0; i < XD_BENCH_MAX_ROWS; i++) {
        g_autofree char *key = g_strdup_printf("Test%zu", i + 1);
        if (i >= settings->test_count) {
            g_key_file_remove_key(file, "Benchmark", key, nullptr);
            continue;
        }
        int values[] = {(int)settings->tests[i].block_size, (int)settings->tests[i].queue_depth,
                        (int)settings->tests[i].threads};
        g_key_file_set_integer_list(file, "Benchmark", key, values, XD_ARRAY_LEN(values));
    }
    g_key_file_set_integer(file, "Benchmark", "MeasureSeconds", (int)settings->measure_seconds);
    g_key_file_set_integer(file, "Benchmark", "IntervalSeconds", (int)settings->interval_seconds);
    g_key_file_set_integer(file, "Benchmark", "Runs", (int)settings->runs);
    g_key_file_set_integer(file, "Benchmark", "Unit", (int)settings->unit);
    g_key_file_set_string(file, "Benchmark", "TestData",
                          settings->random_data ? "random" : "zeros");
    g_autofree char *size = g_strdup_printf("%" G_GUINT64_FORMAT, settings->test_size);
    g_key_file_set_string(file, "Benchmark", "TestSize", size);
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    GError *error = nullptr;
    if (!g_key_file_save_to_file(file, path, &error)) {
        fprintf(stderr, "xd-diskbench: cannot save %s: %s\n", path, error->message);
        g_error_free(error);
    }
}

/* ---- drives ------------------------------------------------------------------------ */

static void free_drive_entry(gpointer data) {
    drive_entry *entry = data;
    g_free(entry->label);
    g_free(entry->name);
    g_free(entry->mount_point);
    g_free(entry->directory);
    g_free(entry->filesystem);
    g_free(entry);
}

static guint64 get_free_bytes(const char *path) {
    struct statvfs fs;
    if (statvfs(path, &fs) != 0)
        return 0;
    return (guint64)fs.f_bavail * fs.f_frsize;
}

static void set_drive_label(drive_entry *entry) {
    g_autofree char *free_text = xd_health_format_data_size(get_free_bytes(entry->directory));
    g_free(entry->label);
    entry->label = g_strdup_printf("%s, %s free", entry->name, free_text);
}

/* File systems that hold no files of their own (or only the kernel's). */
static bool is_pseudo_filesystem(const char *type) {
    static const char *const pseudo[] = {
        "proc",    "sysfs",    "devtmpfs",   "devpts",   "cgroup",  "cgroup2",    "securityfs",
        "pstore",  "efivarfs", "bpf",        "debugfs",  "tracefs", "configfs",   "fusectl",
        "mqueue",  "hugetlbfs", "autofs",    "binfmt_misc", "rpc_pipefs", "nsfs", "ramfs",
        "squashfs", "overlay", "fuse.portal", "fuse.gvfsd-fuse", "iso9660", "udf",
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(pseudo); i++)
        if (strcmp(type, pseudo[i]) == 0)
            return true;
    return false;
}

/* Where a test file goes on the file system mounted at `mount_point`:
 * its root, else the user's home when that's on it; nullptr when
 * neither is writable. */
static char *get_test_directory(const char *mount_point) {
    if (access(mount_point, W_OK) == 0)
        return g_strdup(mount_point);
    const char *home = g_get_home_dir();
    g_autoptr(GUnixMountEntry) home_mount = g_unix_mount_entry_for(home, nullptr);
    if (home_mount != nullptr &&
        strcmp(g_unix_mount_entry_get_mount_path(home_mount), mount_point) == 0 &&
        access(home, W_OK) == 0)
        return g_strdup(home);
    return nullptr;
}

static drive_entry *new_folder_entry(const char *folder) {
    drive_entry *entry = g_new0(drive_entry, 1);
    g_autofree char *base = g_path_get_basename(folder);
    entry->name = g_strdup_printf("%s (%s)", base, folder);
    entry->mount_point = g_strdup(folder);
    /* A drive's root that can't be written: the user's home, if it's on
     * that drive. */
    entry->directory = get_test_directory(folder);
    if (entry->directory == nullptr)
        entry->directory = g_strdup(folder);
    g_autoptr(GUnixMountEntry) mount = g_unix_mount_entry_for(folder, nullptr);
    entry->filesystem =
        g_strdup(mount != nullptr ? g_unix_mount_entry_get_fs_type(mount) : "");
    set_drive_label(entry);
    return entry;
}

static GPtrArray *read_drives(void) {
    GPtrArray *drives = g_ptr_array_new_with_free_func(free_drive_entry);
    GList *mounts = g_unix_mount_entries_get(nullptr);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    for (GList *link = mounts; link != nullptr; link = link->next) {
        GUnixMountEntry *mount = link->data;
        const char *path = g_unix_mount_entry_get_mount_path(mount);
        const char *type = g_unix_mount_entry_get_fs_type(mount);
        if (g_unix_mount_entry_is_readonly(mount) || is_pseudo_filesystem(type) ||
            g_hash_table_contains(seen, path))
            continue;
        /* The system's own (/boot, /var, /run, /proc...), but "/" is the
         * disk most people have. */
        if (g_unix_mount_entry_is_system_internal(mount) && strcmp(path, "/") != 0)
            continue;
        char *directory = get_test_directory(path);
        if (directory == nullptr)
            continue;
        g_hash_table_add(seen, g_strdup(path));
        drive_entry *entry = g_new0(drive_entry, 1);
        g_autofree char *name = g_unix_mount_entry_guess_name(mount);
        entry->name = g_strdup_printf("%s (%s)", name, path);
        entry->mount_point = g_strdup(path);
        entry->directory = directory;
        entry->filesystem = g_strdup(type);
        set_drive_label(entry);
        g_ptr_array_add(drives, entry);
    }
    g_hash_table_unref(seen);
    g_list_free_full(mounts, (GDestroyNotify)g_unix_mount_entry_free);
    return drives;
}

static void fill_drive_combo(diskbench *bench, size_t selected) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(bench->drive_combo));
    for (guint i = 0; i < bench->drives->len; i++) {
        const drive_entry *entry = g_ptr_array_index(bench->drives, i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(bench->drive_combo), entry->label);
    }
    if (bench->drives->len > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(bench->drive_combo), (int)selected);
}

/* Adds `folder` to the list (once) and selects it. */
static void select_folder(diskbench *bench, const char *folder) {
    for (guint i = 0; i < bench->drives->len; i++) {
        const drive_entry *entry = g_ptr_array_index(bench->drives, i);
        if (strcmp(entry->mount_point, folder) == 0 || strcmp(entry->directory, folder) == 0) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(bench->drive_combo), (int)i);
            return;
        }
    }
    if (!g_file_test(folder, G_FILE_TEST_IS_DIR)) {
        fprintf(stderr, "xd-diskbench: %s isn't a folder\n", folder);
        return;
    }
    /* A drive asked for that isn't listed (a read-only root without the
     * user's home on it) still goes in, as a folder. */
    g_ptr_array_add(bench->drives, new_folder_entry(folder));
    fill_drive_combo(bench, bench->drives->len - 1);
}

static drive_entry *get_selected_drive(diskbench *bench) {
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(bench->drive_combo));
    if (index < 0 || (guint)index >= bench->drives->len)
        return nullptr;
    return g_ptr_array_index(bench->drives, (guint)index);
}

static void on_folder_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    diskbench *bench = data;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Browse for Folder", GTK_WINDOW(bench->window), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "OK", "Cancel");
    const drive_entry *entry = get_selected_drive(bench);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                        entry != nullptr ? entry->directory : g_get_home_dir());
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        g_autofree char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (folder != nullptr) {
            if (access(folder, W_OK) != 0)
                xd_message_box(GTK_WINDOW(bench->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                               "You can't write in this folder, so it can't be tested. Choose "
                               "another.");
            else
                select_folder(bench, folder);
        }
    }
    g_object_unref(chooser);
}

/* ---- the results ------------------------------------------------------------------- */

static void set_status(diskbench *bench, const char *text) {
    gtk_label_set_text(GTK_LABEL(bench->status), text);
}

static void show_cell(diskbench *bench, GtkWidget *frame, GtkWidget *label,
                      const xd_bench_measure *measure) {
    g_autofree char *value = xd_bench_format_value(measure, bench->settings.unit);
    g_autofree char *markup = g_markup_printf_escaped("<span size='x-large'>%s</span>", value);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_autofree char *details = xd_bench_format_details(measure);
    gtk_widget_set_tooltip_text(frame, details);
}

static void show_results(diskbench *bench) {
    for (size_t i = 0; i < bench->settings.test_count; i++) {
        result_row *row = &bench->rows[i];
        show_cell(bench, row->read_frame, row->read_label, &row->read_result);
        show_cell(bench, row->write_frame, row->write_label, &row->write_result);
    }
    const char *unit = xd_bench_get_unit_label(bench->settings.unit);
    g_autofree char *read = g_strdup_printf("Read (%s)", unit);
    g_autofree char *write = g_strdup_printf("Write (%s)", unit);
    gtk_label_set_text(GTK_LABEL(bench->read_header), read);
    gtk_label_set_text(GTK_LABEL(bench->write_header), write);
    gtk_widget_set_visible(bench->cached_label, bench->result_cached);
}

static void set_row_titles(diskbench *bench) {
    for (size_t i = 0; i < XD_BENCH_MAX_ROWS; i++) {
        result_row *row = &bench->rows[i];
        bool shown = i < bench->settings.test_count;
        gtk_widget_set_visible(row->button, shown);
        gtk_widget_set_visible(row->read_frame, shown);
        gtk_widget_set_visible(row->write_frame, shown);
        row->read_result = (xd_bench_measure){};
        row->write_result = (xd_bench_measure){};
        if (!shown)
            continue;
        const xd_bench_test *test = &bench->settings.tests[i];
        g_autofree char *title = nullptr;
        g_autofree char *subtitle = nullptr;
        xd_bench_get_test_title(test, &title, &subtitle);
        g_autofree char *markup = g_markup_printf_escaped("<b>%s</b>", title);
        gtk_label_set_markup(GTK_LABEL(row->title), markup);
        gtk_label_set_text(GTK_LABEL(row->subtitle), subtitle);
        g_autofree char *description = xd_bench_describe_test(test);
        gtk_widget_set_tooltip_text(row->button, description);
    }
    bench->result_cached = false;
    show_results(bench);
}

/* ---- running ----------------------------------------------------------------------- */

static void set_running(diskbench *bench, bool running) {
    gtk_widget_set_sensitive(bench->drive_combo, !running);
    gtk_widget_set_sensitive(bench->folder_button, !running);
    gtk_widget_set_sensitive(bench->runs_combo, !running);
    gtk_widget_set_sensitive(bench->size_combo, !running);
    gtk_widget_set_sensitive(bench->settings_button, !running);
    bool all = running && bench->running_row < 0;
    gtk_stack_set_visible_child_name(GTK_STACK(bench->all_stack), all ? "stop" : "test");
    gtk_widget_set_sensitive(bench->all_button, !running || all);
    for (size_t i = 0; i < bench->settings.test_count; i++) {
        result_row *row = &bench->rows[i];
        bool this_row = running && bench->running_row == (int)i;
        gtk_stack_set_visible_child_name(GTK_STACK(row->button_stack),
                                         this_row ? "stop" : "test");
        gtk_widget_set_sensitive(row->button, !running || this_row);
    }
}

static void add_step(xd_bench_plan *plan, size_t row, xd_bench_direction direction,
                     const xd_bench_test *test) {
    XD_ASSERT(plan->step_count < XD_BENCH_MAX_STEPS);
    plan->steps[plan->step_count++] = (xd_bench_step){row, direction, *test};
}

static void describe_step(diskbench *bench, const xd_bench_event *event, char **text) {
    const xd_bench_step *step = &bench->plan.steps[event->step_index];
    g_autofree char *test = xd_bench_describe_test(&step->test);
    const char *doing = step->direction == XD_BENCH_READ    ? "reading"
                        : step->direction == XD_BENCH_WRITE ? "writing"
                                                            : "reading and writing";
    *text = g_strdup_printf("%s: %s, run %u of %u...", test, doing, event->run, bench->plan.runs);
}

static void finish_run(diskbench *bench, const xd_bench_event *event) {
    xd_bench_free_engine(bench->engine);
    bench->engine = nullptr;
    set_running(bench, false);
    if (bench->quit_when_finished) {
        gtk_main_quit();
        return;
    }
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_free(bench->result_date);
    bench->result_date = g_date_time_format(now, "%Y-%m-%d %H:%M");
    if (event->error != nullptr) {
        set_status(bench, "The test stopped with an error.");
        xd_message_box(GTK_WINDOW(bench->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       event->error);
    } else {
        set_status(bench, event->cancelled ? "Stopped. The test file has been deleted."
                                           : "Finished. The test file has been deleted.");
    }
}

static void on_engine_event(const xd_bench_event *event, void *user_data) {
    diskbench *bench = user_data;
    bench->result_cached = bench->result_cached || event->cached;
    switch (event->kind) {
    case XD_BENCH_EVENT_PREPARING: {
        g_autofree char *text = g_strdup_printf("Preparing the test file... %.0f%%",
                                                event->fraction * 100.0);
        set_status(bench, text);
        break;
    }
    case XD_BENCH_EVENT_RUNNING: {
        g_autofree char *text = nullptr;
        describe_step(bench, event, &text);
        set_status(bench, text);
        break;
    }
    case XD_BENCH_EVENT_WAITING: {
        g_autofree char *text =
            g_strdup_printf("Waiting %u seconds before the next test...",
                            bench->plan.interval_ms / 1000);
        set_status(bench, text);
        break;
    }
    case XD_BENCH_EVENT_RESULT: {
        const xd_bench_step *step = &bench->plan.steps[event->step_index];
        result_row *row = &bench->rows[step->row];
        if (step->direction != XD_BENCH_WRITE)
            row->read_result = event->read_result;
        if (step->direction != XD_BENCH_READ)
            row->write_result = event->write_result;
        break;
    }
    case XD_BENCH_EVENT_FINISHED:
        finish_run(bench, event);
        break;
    }
    if (!bench->quit_when_finished)
        show_results(bench);
}

static bool are_settings_equal(const bench_settings *left, const bench_settings *right) {
    if (left->profile != right->profile || left->test_count != right->test_count ||
        left->measure_seconds != right->measure_seconds || left->runs != right->runs ||
        left->random_data != right->random_data || left->test_size != right->test_size)
        return false;
    for (size_t i = 0; i < left->test_count; i++)
        if (left->tests[i].block_size != right->tests[i].block_size ||
            left->tests[i].queue_depth != right->tests[i].queue_depth ||
            left->tests[i].threads != right->tests[i].threads)
            return false;
    return true;
}

/* Starts `row`'s tests, or every row's (-1). */
static void start_run(diskbench *bench, int row_index) {
    drive_entry *entry = get_selected_drive(bench);
    if (entry == nullptr) {
        xd_message_box(GTK_WINDOW(bench->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "There's no drive to test. Choose a folder with Folder....");
        return;
    }
    if (access(entry->directory, W_OK) != 0) {
        g_autofree char *text =
            g_strdup_printf("You can't write in %s, so it can't be tested.", entry->directory);
        xd_message_box(GTK_WINDOW(bench->window), TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, text);
        return;
    }
    const bench_settings *settings = &bench->settings;
    xd_bench_plan *plan = &bench->plan;
    *plan = (xd_bench_plan){
        .directory = entry->directory,
        .file_size = settings->test_size,
        .runs = settings->runs,
        .measure_ms = settings->measure_seconds * 1000,
        .interval_ms = settings->interval_seconds * 1000,
        .random_data = settings->random_data,
    };
    /* All: every row's reads, then their writes (so the reads find the
     * file as it was filled), then the mixed rows. */
    size_t first = row_index < 0 ? 0 : (size_t)row_index;
    size_t last = row_index < 0 ? settings->test_count : first + 1;
    for (size_t i = first; i < last; i++)
        if (settings->tests[i].read_percent == 0)
            add_step(plan, i, XD_BENCH_READ, &settings->tests[i]);
    for (size_t i = first; i < last; i++)
        if (settings->tests[i].read_percent == 0)
            add_step(plan, i, XD_BENCH_WRITE, &settings->tests[i]);
    for (size_t i = first; i < last; i++)
        if (settings->tests[i].read_percent > 0)
            add_step(plan, i, XD_BENCH_MIXED, &settings->tests[i]);
    for (size_t i = first; i < last; i++) {
        bench->rows[i].read_result = (xd_bench_measure){};
        bench->rows[i].write_result = (xd_bench_measure){};
    }
    /* Results measured another way don't mix with these. */
    if (row_index < 0 || bench->result_drive == nullptr ||
        strcmp(bench->result_drive, entry->label) != 0 ||
        !are_settings_equal(&bench->result_settings, settings)) {
        for (size_t i = 0; i < settings->test_count; i++) {
            bench->rows[i].read_result = (xd_bench_measure){};
            bench->rows[i].write_result = (xd_bench_measure){};
        }
        bench->result_cached = false;
    }
    g_free(bench->result_drive);
    bench->result_drive = g_strdup(entry->label);
    g_free(bench->result_filesystem);
    bench->result_filesystem = g_strdup(entry->filesystem);
    bench->result_settings = *settings;
    bench->running_row = row_index;
    show_results(bench);
    set_running(bench, true);
    set_status(bench, "Preparing the test file...");
    bench->engine = xd_bench_start(plan, on_engine_event, bench);
}

static void on_test_clicked(GtkButton *button, gpointer data) {
    diskbench *bench = data;
    if (bench->engine != nullptr) {
        set_status(bench, "Stopping...");
        xd_bench_stop(bench->engine);
        return;
    }
    int row = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "xd-row"));
    start_run(bench, row);
}

/* ---- the Settings dialog ----------------------------------------------------------- */

typedef struct settings_dialog {
    GtkWidget *dialog, *profile_combo, *measure_spin, *interval_spin, *random_radio, *zeros_radio;
    GtkWidget *row_labels[XD_BENCH_MAX_ROWS], *block_combos[XD_BENCH_MAX_ROWS],
        *queue_spins[XD_BENCH_MAX_ROWS], *thread_spins[XD_BENCH_MAX_ROWS];
    bench_settings settings;
} settings_dialog;

static void show_dialog_tests(settings_dialog *dialog) {
    for (size_t i = 0; i < XD_BENCH_MAX_ROWS; i++) {
        bool shown = i < dialog->settings.test_count;
        gtk_widget_set_visible(dialog->row_labels[i], shown);
        gtk_widget_set_visible(dialog->block_combos[i], shown);
        gtk_widget_set_visible(dialog->queue_spins[i], shown);
        gtk_widget_set_visible(dialog->thread_spins[i], shown);
        if (!shown)
            continue;
        const xd_bench_test *test = &dialog->settings.tests[i];
        g_autofree char *title = nullptr;
        g_autofree char *subtitle = nullptr;
        xd_bench_get_test_title(test, &title, &subtitle);
        /* The title's block size is the combo's; the pattern stays. */
        const char *kind = test->read_percent > 0          ? "Mixed 70/30"
                           : test->pattern == XD_BENCH_RANDOM ? "Random"
                                                               : "Sequential";
        g_autofree char *label = g_strdup_printf("%zu. %s", i + 1, kind);
        gtk_label_set_text(GTK_LABEL(dialog->row_labels[i]), label);
        for (size_t k = 0; k < xd_bench_block_size_count; k++)
            if (xd_bench_block_sizes[k] == test->block_size)
                gtk_combo_box_set_active(GTK_COMBO_BOX(dialog->block_combos[i]), (int)k);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->queue_spins[i]), test->queue_depth);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->thread_spins[i]), test->threads);
    }
}

static void show_dialog_settings(settings_dialog *dialog) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(dialog->profile_combo), dialog->settings.profile);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->measure_spin),
                              dialog->settings.measure_seconds);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dialog->interval_spin),
                              dialog->settings.interval_seconds);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(dialog->settings.random_data ? dialog->random_radio
                                                       : dialog->zeros_radio),
        TRUE);
    show_dialog_tests(dialog);
}

static void on_profile_changed(GtkComboBox *combo, gpointer data) {
    settings_dialog *dialog = data;
    int profile = gtk_combo_box_get_active(combo);
    if (profile < 0 || profile == dialog->settings.profile)
        return;
    dialog->settings.profile = (xd_bench_profile)profile;
    dialog->settings.test_count =
        xd_bench_get_profile_tests(dialog->settings.profile, dialog->settings.tests);
    show_dialog_tests(dialog);
}

static void read_dialog_settings(settings_dialog *dialog) {
    bench_settings *settings = &dialog->settings;
    for (size_t i = 0; i < settings->test_count; i++) {
        int block = gtk_combo_box_get_active(GTK_COMBO_BOX(dialog->block_combos[i]));
        if (block >= 0)
            settings->tests[i].block_size = xd_bench_block_sizes[block];
        settings->tests[i].queue_depth = (uint32_t)gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(dialog->queue_spins[i]));
        settings->tests[i].threads = (uint32_t)gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(dialog->thread_spins[i]));
    }
    settings->measure_seconds =
        (unsigned)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->measure_spin));
    settings->interval_seconds =
        (unsigned)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(dialog->interval_spin));
    settings->random_data =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(dialog->random_radio));
}

static GtkWidget *new_spin(double min, double max) {
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(spin), 4);
    return spin;
}

static GtkWidget *new_group(const char *title, GtkWidget *child) {
    GtkWidget *frame = gtk_frame_new(title);
    gtk_container_set_border_width(GTK_CONTAINER(child), 8);
    gtk_container_add(GTK_CONTAINER(frame), child);
    return frame;
}

static GtkWidget *new_label(const char *mnemonic, GtkWidget *target) {
    GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    if (target != nullptr)
        gtk_label_set_mnemonic_widget(GTK_LABEL(label), target);
    return label;
}

enum { RESPONSE_RESET = 1 };

static void open_settings_dialog(diskbench *bench) {
    settings_dialog dialog = {.settings = bench->settings};
    dialog.dialog = gtk_dialog_new_with_buttons(
        "Settings", GTK_WINDOW(bench->window),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), "_Reset", RESPONSE_RESET, "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog.dialog), GTK_RESPONSE_OK);
    gtk_window_set_resizable(GTK_WINDOW(dialog.dialog), FALSE);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog.dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_set_spacing(GTK_BOX(content), 10);

    GtkWidget *profile_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    dialog.profile_combo = gtk_combo_box_text_new();
    for (int i = 0; i < XD_BENCH_PROFILE_COUNT; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dialog.profile_combo),
                                       xd_bench_get_profile_name((xd_bench_profile)i));
    gtk_box_pack_start(GTK_BOX(profile_row), new_label("_Profile:", dialog.profile_combo), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(profile_row), dialog.profile_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), profile_row, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    const char *const headers[] = {"Test", "Block size", "Queue depth", "Threads"};
    for (int i = 0; i < 4; i++)
        gtk_grid_attach(GTK_GRID(grid), new_label(headers[i], nullptr), i, 0, 1, 1);
    for (int i = 0; i < XD_BENCH_MAX_ROWS; i++) {
        dialog.row_labels[i] = new_label("", nullptr);
        dialog.block_combos[i] = gtk_combo_box_text_new();
        for (size_t k = 0; k < xd_bench_block_size_count; k++) {
            g_autofree char *size = xd_bench_format_size(xd_bench_block_sizes[k]);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dialog.block_combos[i]), size);
        }
        dialog.queue_spins[i] = new_spin(1, XD_BENCH_MAX_QUEUE_DEPTH);
        dialog.thread_spins[i] = new_spin(1, XD_BENCH_MAX_THREADS);
        gtk_grid_attach(GTK_GRID(grid), dialog.row_labels[i], 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), dialog.block_combos[i], 1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), dialog.queue_spins[i], 2, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), dialog.thread_spins[i], 3, i + 1, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(content), new_group("Tests", grid), FALSE, FALSE, 0);

    GtkWidget *timing = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(timing), 6);
    gtk_grid_set_column_spacing(GTK_GRID(timing), 8);
    dialog.measure_spin = new_spin(1, MAX_SECONDS);
    dialog.interval_spin = new_spin(0, MAX_SECONDS);
    gtk_grid_attach(GTK_GRID(timing), new_label("_Measure each run for:", dialog.measure_spin), 0,
                    0, 1, 1);
    gtk_grid_attach(GTK_GRID(timing), dialog.measure_spin, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(timing), new_label("seconds", nullptr), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(timing),
                    new_label("_Wait between tests:", dialog.interval_spin), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(timing), dialog.interval_spin, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(timing), new_label("seconds", nullptr), 2, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(content), new_group("Timing", timing), FALSE, FALSE, 0);

    GtkWidget *data_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    dialog.random_radio = gtk_radio_button_new_with_mnemonic(nullptr, "R_andom");
    dialog.zeros_radio = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(dialog.random_radio), "_Zeros");
    gtk_box_pack_start(GTK_BOX(data_box), dialog.random_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(data_box), dialog.zeros_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), new_group("Test data", data_box), FALSE, FALSE, 0);

    gtk_widget_show_all(content);
    show_dialog_settings(&dialog);
    g_signal_connect(dialog.profile_combo, "changed", G_CALLBACK(on_profile_changed), &dialog);
    int response;
    while ((response = gtk_dialog_run(GTK_DIALOG(dialog.dialog))) == RESPONSE_RESET) {
        bench_settings defaults;
        set_default_settings(&defaults);
        dialog.settings.profile = defaults.profile;
        dialog.settings.test_count = defaults.test_count;
        memcpy(dialog.settings.tests, defaults.tests, sizeof(defaults.tests));
        dialog.settings.measure_seconds = defaults.measure_seconds;
        dialog.settings.interval_seconds = defaults.interval_seconds;
        dialog.settings.random_data = defaults.random_data;
        show_dialog_settings(&dialog);
    }
    if (response == GTK_RESPONSE_OK) {
        read_dialog_settings(&dialog);
        bench->settings = dialog.settings;
        save_settings(bench);
        set_row_titles(bench);
    }
    gtk_widget_destroy(dialog.dialog);
}

static void on_settings_clicked([[maybe_unused]] GtkButton *button, gpointer data) {
    open_settings_dialog(data);
}

/* ---- the main window's choices ----------------------------------------------------- */

static void on_runs_changed(GtkComboBox *combo, gpointer data) {
    diskbench *bench = data;
    bench->settings.runs = (unsigned)gtk_combo_box_get_active(combo) + 1;
    save_settings(bench);
}

static void on_size_changed(GtkComboBox *combo, gpointer data) {
    diskbench *bench = data;
    int index = gtk_combo_box_get_active(combo);
    if (index >= 0)
        bench->settings.test_size = xd_bench_test_sizes[index];
    save_settings(bench);
}

static void on_unit_changed(GtkComboBox *combo, gpointer data) {
    diskbench *bench = data;
    int index = gtk_combo_box_get_active(combo);
    if (index >= 0)
        bench->settings.unit = (xd_bench_unit)index;
    save_settings(bench);
    show_results(bench);
}

/* ---- Copy -------------------------------------------------------------------------- */

static char *format_benchmark_text(diskbench *bench) {
    const bench_settings *settings =
        bench->result_drive != nullptr ? &bench->result_settings : &bench->settings;
    const drive_entry *entry = get_selected_drive(bench);
    xd_bench_test tests[XD_BENCH_MAX_ROWS];
    xd_bench_measure reads[XD_BENCH_MAX_ROWS], writes[XD_BENCH_MAX_ROWS];
    for (size_t i = 0; i < settings->test_count; i++) {
        tests[i] = settings->tests[i];
        reads[i] = bench->rows[i].read_result;
        writes[i] = bench->rows[i].write_result;
    }
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *today = g_date_time_format(now, "%Y-%m-%d %H:%M");
    xd_bench_report report = {
        .drive = bench->result_drive != nullptr ? bench->result_drive
                 : entry != nullptr             ? entry->label
                                                : "",
        .filesystem = bench->result_drive != nullptr ? bench->result_filesystem
                      : entry != nullptr             ? entry->filesystem
                                                     : "",
        .date = bench->result_date != nullptr ? bench->result_date : today,
        .test_size = settings->test_size,
        .runs = settings->runs,
        .measure_seconds = settings->measure_seconds,
        .interval_seconds = settings->interval_seconds,
        .random_data = settings->random_data,
        .cached = bench->result_cached,
        .profile = settings->profile,
        .unit = bench->settings.unit,
        .row_count = settings->test_count,
        .tests = tests,
        .read_results = reads,
        .write_results = writes,
    };
    return xd_bench_format_report(&report);
}

static void on_copy(GtkMenuItem *item, gpointer data) {
    (void)item;
    diskbench *bench = data;
    g_autofree char *text = gtk_notebook_get_current_page(GTK_NOTEBOOK(bench->notebook)) == 1
                                ? health_format_text(bench->health)
                                : format_benchmark_text(bench);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
}

static void on_refresh(GtkMenuItem *item, gpointer data) {
    (void)item;
    diskbench *bench = data;
    if (gtk_notebook_get_current_page(GTK_NOTEBOOK(bench->notebook)) == 1) {
        health_refresh(bench->health);
    } else if (bench->engine == nullptr) {
        int index = gtk_combo_box_get_active(GTK_COMBO_BOX(bench->drive_combo));
        for (guint i = 0; i < bench->drives->len; i++)
            set_drive_label(g_ptr_array_index(bench->drives, i));
        fill_drive_combo(bench, index < 0 ? 0 : (size_t)index);
    }
}

/* ---- the window -------------------------------------------------------------------- */

/* Closing while a test runs stops it and waits for the test file to be
 * deleted. */
static gboolean on_delete(GtkWidget *widget, [[maybe_unused]] GdkEvent *event, gpointer data) {
    diskbench *bench = data;
    if (bench->engine == nullptr)
        return FALSE;
    bench->quit_when_finished = true;
    xd_bench_stop(bench->engine);
    gtk_widget_hide(widget);
    return TRUE;
}

static void on_exit_activate(GtkMenuItem *item, gpointer data) {
    (void)item;
    diskbench *bench = data;
    if (!on_delete(bench->window, nullptr, bench))
        gtk_widget_destroy(bench->window);
}

static GtkWidget *add_menu(GtkWidget *bar, const char *label) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), item);
    return menu;
}

static void add_menu_item(GtkWidget *menu, const char *label, GCallback callback, gpointer data,
                          GtkAccelGroup *accel, guint key, GdkModifierType modifiers) {
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", callback, data);
    if (accel != nullptr)
        gtk_widget_add_accelerator(item, "activate", accel, key, modifiers, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static GtkWidget *build_menu_bar(diskbench *bench) {
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(bench->window), accel);
    GtkWidget *bar = gtk_menu_bar_new();
    GtkWidget *file = add_menu(bar, "_File");
    add_menu_item(file, "E_xit", G_CALLBACK(on_exit_activate), bench, nullptr, 0,
                  (GdkModifierType)0);
    GtkWidget *edit = add_menu(bar, "_Edit");
    add_menu_item(edit, "_Copy", G_CALLBACK(on_copy), bench, accel, GDK_KEY_c,
                  GDK_CONTROL_MASK);
    GtkWidget *view = add_menu(bar, "_View");
    add_menu_item(view, "_Refresh", G_CALLBACK(on_refresh), bench, accel, GDK_KEY_F5,
                  (GdkModifierType)0);
    g_object_unref(accel);
    return bar;
}

/* A test's button: its name over its queue and threads, or Stop. */
static GtkWidget *new_test_button(diskbench *bench, int row, GtkWidget **stack,
                                  GtkWidget **title, GtkWidget **subtitle) {
    GtkWidget *button = gtk_button_new();
    *stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(*stack), TRUE);
    GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    *title = gtk_label_new("");
    *subtitle = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(labels), *title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), *subtitle, FALSE, FALSE, 0);
    gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
    gtk_stack_add_named(GTK_STACK(*stack), labels, "test");
    /* Alt+S stops it: Settings... is greyed while a test runs. */
    GtkWidget *stop_label = gtk_label_new_with_mnemonic("_Stop");
    gtk_label_set_mnemonic_widget(GTK_LABEL(stop_label), button);
    gtk_stack_add_named(GTK_STACK(*stack), stop_label, "stop");
    gtk_container_add(GTK_CONTAINER(button), *stack);
    gtk_widget_set_size_request(button, 170, 52);
    g_object_set_data(G_OBJECT(button), "xd-row", GINT_TO_POINTER(row));
    g_signal_connect(button, "clicked", G_CALLBACK(on_test_clicked), bench);
    return button;
}

static GtkWidget *new_result_cell(GtkWidget **label) {
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(*label), 1.0);
    gtk_widget_set_margin_start(*label, 8);
    gtk_widget_set_margin_end(*label, 8);
    gtk_container_add(GTK_CONTAINER(frame), *label);
    gtk_widget_set_size_request(frame, 150, -1);
    gtk_widget_set_hexpand(frame, TRUE);
    return frame;
}

static GtkWidget *new_combo(const char *const *items, size_t count, size_t active) {
    GtkWidget *combo = gtk_combo_box_text_new();
    for (size_t i = 0; i < count; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), items[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), (int)active);
    return combo;
}

static GtkWidget *build_benchmark_page(diskbench *bench) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    bench->drive_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(bench->drive_combo, TRUE);
    bench->folder_button = gtk_button_new_with_mnemonic("_Folder...");
    g_signal_connect(bench->folder_button, "clicked", G_CALLBACK(on_folder_clicked), bench);
    gtk_grid_attach(GTK_GRID(grid), new_label("_Drive:", bench->drive_combo), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bench->drive_combo, 1, 0, 6, 1);
    gtk_grid_attach(GTK_GRID(grid), bench->folder_button, 7, 0, 1, 1);

    const char *runs[MAX_RUNS];
    char run_text[MAX_RUNS][4];
    for (unsigned i = 0; i < MAX_RUNS; i++) {
        g_snprintf(run_text[i], sizeof(run_text[i]), "%u", i + 1);
        runs[i] = run_text[i];
    }
    bench->runs_combo = new_combo(runs, MAX_RUNS, bench->settings.runs - 1);
    char **sizes = g_new0(char *, xd_bench_test_size_count + 1);
    size_t size_index = 0;
    for (size_t i = 0; i < xd_bench_test_size_count; i++) {
        sizes[i] = xd_bench_format_size(xd_bench_test_sizes[i]);
        if (xd_bench_test_sizes[i] == bench->settings.test_size)
            size_index = i;
    }
    bench->size_combo =
        new_combo((const char *const *)sizes, xd_bench_test_size_count, size_index);
    g_strfreev(sizes);
    const char *units[XD_BENCH_UNIT_COUNT];
    for (int i = 0; i < XD_BENCH_UNIT_COUNT; i++)
        units[i] = xd_bench_get_unit_label((xd_bench_unit)i);
    bench->unit_combo = new_combo(units, XD_BENCH_UNIT_COUNT, bench->settings.unit);
    bench->settings_button = gtk_button_new_with_mnemonic("_Settings...");
    g_signal_connect(bench->settings_button, "clicked", G_CALLBACK(on_settings_clicked), bench);
    gtk_grid_attach(GTK_GRID(grid), new_label("R_uns:", bench->runs_combo), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bench->runs_combo, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), new_label("Test si_ze:", bench->size_combo), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bench->size_combo, 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), new_label("U_nits:", bench->unit_combo), 4, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bench->unit_combo, 5, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bench->settings_button, 7, 1, 1, 1);
    g_signal_connect(bench->runs_combo, "changed", G_CALLBACK(on_runs_changed), bench);
    g_signal_connect(bench->size_combo, "changed", G_CALLBACK(on_size_changed), bench);
    g_signal_connect(bench->unit_combo, "changed", G_CALLBACK(on_unit_changed), bench);
    gtk_box_pack_start(GTK_BOX(page), new_group("Settings", grid), FALSE, FALSE, 0);

    bench->results_grid = gtk_grid_new();
    GtkWidget *results = bench->results_grid;
    gtk_grid_set_row_spacing(GTK_GRID(results), 6);
    gtk_grid_set_column_spacing(GTK_GRID(results), 10);
    bench->all_button = gtk_button_new();
    bench->all_stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(bench->all_stack), TRUE);
    GtkWidget *all_label = gtk_label_new(nullptr);
    gtk_label_set_markup_with_mnemonic(GTK_LABEL(all_label), "<span size='large'><b>_All</b></span>");
    gtk_label_set_mnemonic_widget(GTK_LABEL(all_label), bench->all_button);
    GtkWidget *stop_label = gtk_label_new(nullptr);
    gtk_label_set_markup_with_mnemonic(GTK_LABEL(stop_label),
                                       "<span size='large'><b>_Stop</b></span>");
    gtk_label_set_mnemonic_widget(GTK_LABEL(stop_label), bench->all_button);
    gtk_stack_add_named(GTK_STACK(bench->all_stack), all_label, "test");
    gtk_stack_add_named(GTK_STACK(bench->all_stack), stop_label, "stop");
    gtk_container_add(GTK_CONTAINER(bench->all_button), bench->all_stack);
    gtk_widget_set_size_request(bench->all_button, 170, 44);
    g_object_set_data(G_OBJECT(bench->all_button), "xd-row", GINT_TO_POINTER(-1));
    g_signal_connect(bench->all_button, "clicked", G_CALLBACK(on_test_clicked), bench);
    gtk_grid_attach(GTK_GRID(results), bench->all_button, 0, 0, 1, 1);
    bench->read_header = gtk_label_new("");
    bench->write_header = gtk_label_new("");
    gtk_widget_set_valign(bench->read_header, GTK_ALIGN_END);
    gtk_widget_set_valign(bench->write_header, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(results), bench->read_header, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(results), bench->write_header, 2, 0, 1, 1);
    for (int i = 0; i < XD_BENCH_MAX_ROWS; i++) {
        result_row *row = &bench->rows[i];
        row->button = new_test_button(bench, i, &row->button_stack, &row->title, &row->subtitle);
        row->read_frame = new_result_cell(&row->read_label);
        row->write_frame = new_result_cell(&row->write_label);
        gtk_grid_attach(GTK_GRID(results), row->button, 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(results), row->read_frame, 1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(results), row->write_frame, 2, i + 1, 1, 1);
    }
    bench->cached_label = gtk_label_new("(cached)");
    gtk_label_set_xalign(GTK_LABEL(bench->cached_label), 1.0);
    gtk_widget_set_tooltip_text(bench->cached_label,
                                "This file system doesn't allow direct I/O, so the test used the "
                                "page cache, which can make reads look faster than the disk is.");
    gtk_widget_set_no_show_all(bench->cached_label, TRUE);
    gtk_grid_attach(GTK_GRID(results), bench->cached_label, 1, XD_BENCH_MAX_ROWS + 1, 2, 1);
    gtk_box_pack_start(GTK_BOX(page), new_group("Results", results), FALSE, FALSE, 0);
    return page;
}

int main(int argc, char **argv) {
    int page = 0;
    const char *drive = nullptr, *folder = nullptr, *device = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            page = atoi(argv[++i]);
        else if (strcmp(argv[i], "--drive") == 0 && i + 1 < argc)
            drive = argv[++i];
        else if (strcmp(argv[i], "--folder") == 0 && i + 1 < argc)
            folder = argv[++i];
        else if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc)
            device = argv[++i];
    }
    xd_gtk_app_preinit("org.xeondesktop.DiskBenchmark");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();

    static diskbench bench;
    bench.running_row = -1;
    load_settings(&bench.settings);
    bench.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(bench.window), TITLE);
    gtk_window_set_icon_name(GTK_WINDOW(bench.window), "drive-harddisk");
    gtk_window_set_default_size(GTK_WINDOW(bench.window), 640, 620);
    g_signal_connect(bench.window, "delete-event", G_CALLBACK(on_delete), &bench);
    g_signal_connect(bench.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(bench.window), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), build_menu_bar(&bench), FALSE, FALSE, 0);

    bench.status = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(bench.status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(bench.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(bench.status, 6);
    gtk_widget_set_margin_top(bench.status, 2);
    gtk_widget_set_margin_bottom(bench.status, 2);

    bench.notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(bench.notebook), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(bench.notebook), build_benchmark_page(&bench),
                             gtk_label_new("Benchmark"));
    bench.health = health_init_tab(GTK_WINDOW(bench.window), GTK_LABEL(bench.status),
                                   drive != nullptr ? drive : folder, device);
    gtk_notebook_append_page(GTK_NOTEBOOK(bench.notebook), health_get_widget(bench.health),
                             gtk_label_new("Health"));
    gtk_box_pack_start(GTK_BOX(vbox), bench.notebook, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bench.status, FALSE, FALSE, 0);
    xd_gtk_notebook_ctrl_tab(bench.window, GTK_NOTEBOOK(bench.notebook));

    bench.drives = read_drives();
    fill_drive_combo(&bench, 0);
    if (drive != nullptr)
        select_folder(&bench, drive);
    if (folder != nullptr)
        select_folder(&bench, folder);

    gtk_widget_show_all(bench.window);
    set_row_titles(&bench);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(bench.notebook), XD_CLAMP(page, 0, 1));
    gtk_main();
    health_free_tab(bench.health);
    g_ptr_array_unref(bench.drives);
    g_free(bench.result_drive);
    g_free(bench.result_filesystem);
    g_free(bench.result_date);
    return EXIT_SUCCESS;
}
