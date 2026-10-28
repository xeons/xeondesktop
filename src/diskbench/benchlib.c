/* SPDX-License-Identifier: MIT */
#include "benchlib.h"

#include <inttypes.h>

#include "../core/common.h"

static constexpr uint32_t KIB = 1024;
static constexpr uint32_t MIB = 1024 * 1024;

/* ---- the tests ---------------------------------------------------------------------- */

const char *xd_bench_get_profile_name(xd_bench_profile profile) {
    switch (profile) {
    case XD_BENCH_PROFILE_PEAK:
        return "Peak performance";
    case XD_BENCH_PROFILE_REALISTIC:
        return "Realistic";
    case XD_BENCH_PROFILE_DEFAULT:
    case XD_BENCH_PROFILE_COUNT:
        break;
    }
    return "Default";
}

size_t xd_bench_get_profile_tests(xd_bench_profile profile,
                                  xd_bench_test tests[XD_BENCH_MAX_ROWS]) {
    /* Peak performance asks for deeper queues and more threads, as a
     * server's load would; Realistic for the shallow queues of a desktop's,
     * with a row mixing reads and writes as programs do. */
    static const xd_bench_test default_tests[] = {
        {XD_BENCH_SEQUENTIAL, MIB, 8, 1, 0},
        {XD_BENCH_SEQUENTIAL, MIB, 1, 1, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 32, 1, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 1, 1, 0},
    };
    static const xd_bench_test peak_tests[] = {
        {XD_BENCH_SEQUENTIAL, MIB, 32, 1, 0},
        {XD_BENCH_SEQUENTIAL, 128 * KIB, 32, 4, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 64, 4, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 32, 16, 0},
    };
    static const xd_bench_test realistic_tests[] = {
        {XD_BENCH_SEQUENTIAL, MIB, 8, 1, 0},
        {XD_BENCH_SEQUENTIAL, MIB, 1, 1, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 4, 1, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 1, 1, 0},
        {XD_BENCH_RANDOM, 4 * KIB, 1, 1, 70},
    };
    const xd_bench_test *source = default_tests;
    size_t count = XD_ARRAY_LEN(default_tests);
    switch (profile) {
    case XD_BENCH_PROFILE_PEAK:
        source = peak_tests;
        count = XD_ARRAY_LEN(peak_tests);
        break;
    case XD_BENCH_PROFILE_REALISTIC:
        source = realistic_tests;
        count = XD_ARRAY_LEN(realistic_tests);
        break;
    case XD_BENCH_PROFILE_DEFAULT:
    case XD_BENCH_PROFILE_COUNT:
        break;
    }
    static_assert(XD_ARRAY_LEN(realistic_tests) <= XD_BENCH_MAX_ROWS);
    memcpy(tests, source, count * sizeof(*tests));
    return count;
}

static const char *get_pattern_name(xd_bench_pattern pattern) {
    return pattern == XD_BENCH_RANDOM ? "Random" : "Sequential";
}

char *xd_bench_describe_test(const xd_bench_test *test) {
    g_autofree char *block = xd_bench_format_size(test->block_size);
    const char *threads = test->threads == 1 ? "thread" : "threads";
    if (test->read_percent > 0 && test->read_percent < 100)
        return g_strdup_printf("Mixed %u/%u read/write, %s, %s blocks, queue depth %u, %u %s",
                               test->read_percent, 100u - test->read_percent,
                               test->pattern == XD_BENCH_RANDOM ? "random" : "sequential", block,
                               test->queue_depth, test->threads, threads);
    return g_strdup_printf("%s, %s blocks, queue depth %u, %u %s", get_pattern_name(test->pattern),
                           block, test->queue_depth, test->threads, threads);
}

void xd_bench_get_test_title(const xd_bench_test *test, char **title, char **subtitle) {
    g_autofree char *block = xd_bench_format_size(test->block_size);
    if (test->read_percent > 0 && test->read_percent < 100)
        *title = g_strdup_printf("Mixed %u/%u %s", test->read_percent,
                                 100u - test->read_percent, block);
    else
        *title = g_strdup_printf("%s %s", get_pattern_name(test->pattern), block);
    *subtitle = g_strdup_printf("Queue depth %u, %u %s", test->queue_depth, test->threads,
                                test->threads == 1 ? "thread" : "threads");
}

bool xd_bench_is_test_valid(const xd_bench_test *test) {
    uint32_t block = test->block_size;
    bool power_of_two = block != 0 && (block & (block - 1)) == 0;
    return power_of_two && block >= 512 && block <= 64 * MIB && test->queue_depth >= 1 &&
           test->queue_depth <= XD_BENCH_MAX_QUEUE_DEPTH && test->threads >= 1 &&
           test->threads <= XD_BENCH_MAX_THREADS && test->read_percent < 100;
}

const uint64_t xd_bench_test_sizes[] = {
    (uint64_t)16 * MIB,  (uint64_t)32 * MIB,  (uint64_t)64 * MIB,   (uint64_t)128 * MIB,
    (uint64_t)256 * MIB, (uint64_t)512 * MIB, (uint64_t)1 << 30,    (uint64_t)2 << 30,
    (uint64_t)4 << 30,   (uint64_t)8 << 30,   (uint64_t)16 << 30,   (uint64_t)32 << 30,
    (uint64_t)64 << 30,
};
const size_t xd_bench_test_size_count = XD_ARRAY_LEN(xd_bench_test_sizes);

const uint32_t xd_bench_block_sizes[] = {
    4 * KIB,   8 * KIB,  16 * KIB, 32 * KIB, 64 * KIB, 128 * KIB,
    256 * KIB, 512 * KIB, MIB,     2 * MIB,  4 * MIB,  8 * MIB,
};
const size_t xd_bench_block_size_count = XD_ARRAY_LEN(xd_bench_block_sizes);

char *xd_bench_format_size(uint64_t bytes) {
    static const char *const units[] = {"KiB", "MiB", "GiB", "TiB"};
    if (bytes < KIB)
        return g_strdup_printf("%" PRIu64 " bytes", bytes);
    double value = (double)bytes;
    size_t unit = 0;
    uint64_t divisor = KIB;
    while (unit + 1 < XD_ARRAY_LEN(units) && bytes >= divisor * KIB) {
        divisor *= KIB;
        unit++;
    }
    if (bytes % divisor == 0)
        return g_strdup_printf("%" PRIu64 " %s", bytes / divisor, units[unit]);
    return g_strdup_printf("%.1f %s", value / (double)divisor, units[unit]);
}

/* ---- offsets ----------------------------------------------------------------------- */

uint64_t xd_bench_get_next_random(uint64_t *state) {
    uint64_t value = (*state += 0x9e3779b97f4a7c15u);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9u;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebu;
    return value ^ (value >> 31);
}

bool xd_bench_choose_read(uint64_t *state, unsigned read_percent) {
    return xd_bench_get_next_random(state) % 100 < read_percent;
}

void xd_bench_init_offsets(xd_bench_offsets *offsets, xd_bench_pattern pattern,
                           uint64_t block_size, uint64_t first_block, uint64_t block_count,
                           uint64_t seed) {
    XD_ASSERT(block_size > 0 && block_count > 0);
    *offsets = (xd_bench_offsets){
        .pattern = pattern,
        .block_size = block_size,
        .first_block = first_block,
        .block_count = block_count,
        .position = 0,
        .random_state = seed,
    };
}

uint64_t xd_bench_get_next_offset(xd_bench_offsets *offsets) {
    uint64_t block;
    if (offsets->pattern == XD_BENCH_RANDOM) {
        /* The modulo's bias is under one in 2^40 for any test file here. */
        block = xd_bench_get_next_random(&offsets->random_state) % offsets->block_count;
    } else {
        block = offsets->position;
        offsets->position = (offsets->position + 1) % offsets->block_count;
    }
    return (offsets->first_block + block) * offsets->block_size;
}

/* ---- statistics -------------------------------------------------------------------- */

bool xd_bench_has_result(const xd_bench_measure *measure) {
    return measure->operations > 0 && measure->elapsed_ns > 0;
}

double xd_bench_get_megabytes_per_second(const xd_bench_measure *measure) {
    if (!xd_bench_has_result(measure))
        return 0;
    return (double)measure->bytes / 1e6 / ((double)measure->elapsed_ns / 1e9);
}

double xd_bench_get_iops(const xd_bench_measure *measure) {
    if (!xd_bench_has_result(measure))
        return 0;
    return (double)measure->operations / ((double)measure->elapsed_ns / 1e9);
}

double xd_bench_get_mean_latency_us(const xd_bench_measure *measure) {
    if (measure->operations == 0)
        return 0;
    return (double)measure->latency_ns / (double)measure->operations / 1e3;
}

bool xd_bench_is_better(const xd_bench_measure *candidate, const xd_bench_measure *best) {
    if (!xd_bench_has_result(candidate))
        return false;
    if (!xd_bench_has_result(best))
        return true;
    return xd_bench_get_megabytes_per_second(candidate) > xd_bench_get_megabytes_per_second(best);
}

const char *xd_bench_get_unit_label(xd_bench_unit unit) {
    switch (unit) {
    case XD_BENCH_UNIT_GIGABYTES:
        return "GB/s";
    case XD_BENCH_UNIT_IOPS:
        return "IOPS";
    case XD_BENCH_UNIT_MICROSECONDS:
        return "μs";
    case XD_BENCH_UNIT_MEGABYTES:
    case XD_BENCH_UNIT_COUNT:
        break;
    }
    return "MB/s";
}

char *xd_bench_format_value(const xd_bench_measure *measure, xd_bench_unit unit) {
    if (!xd_bench_has_result(measure))
        return g_strdup("");
    switch (unit) {
    case XD_BENCH_UNIT_GIGABYTES:
        return g_strdup_printf("%.3f", xd_bench_get_megabytes_per_second(measure) / 1000.0);
    case XD_BENCH_UNIT_IOPS:
        return g_strdup_printf("%.2f", xd_bench_get_iops(measure));
    case XD_BENCH_UNIT_MICROSECONDS:
        return g_strdup_printf("%.2f", xd_bench_get_mean_latency_us(measure));
    case XD_BENCH_UNIT_MEGABYTES:
    case XD_BENCH_UNIT_COUNT:
        break;
    }
    return g_strdup_printf("%.2f", xd_bench_get_megabytes_per_second(measure));
}

char *xd_bench_format_details(const xd_bench_measure *measure) {
    if (!xd_bench_has_result(measure))
        return g_strdup("Not measured yet");
    return g_strdup_printf("%.2f MB/s\n%.2f IOPS\n%.2f μs average latency",
                           xd_bench_get_megabytes_per_second(measure), xd_bench_get_iops(measure),
                           xd_bench_get_mean_latency_us(measure));
}

/* ---- Copy's text -------------------------------------------------------------------- */

/* A cell of the table: the value and its unit, or "-" when not run. */
static char *format_cell(const xd_bench_measure *measure, xd_bench_unit unit) {
    if (!xd_bench_has_result(measure))
        return g_strdup("-");
    g_autofree char *value = xd_bench_format_value(measure, unit);
    return g_strdup_printf("%s %s", value, xd_bench_get_unit_label(unit));
}

/* Pads to `width` columns, counting characters rather than bytes (μs). */
static void append_padded(GString *text, const char *cell, long width, bool right) {
    long length = g_utf8_strlen(cell, -1);
    if (right)
        for (long i = length; i < width; i++)
            g_string_append_c(text, ' ');
    g_string_append(text, cell);
    if (!right)
        for (long i = length; i < width; i++)
            g_string_append_c(text, ' ');
}

char *xd_bench_format_report(const xd_bench_report *report) {
    GString *text = g_string_new("Disk Benchmark and Health: benchmark results\n");
    g_string_append_printf(text, "Date: %s\n", report->date);
    g_string_append_printf(text, "Drive: %s\n", report->drive);
    if (report->filesystem != nullptr && report->filesystem[0] != '\0')
        g_string_append_printf(text, "File system: %s%s\n", report->filesystem,
                               report->cached ? " (cached: it doesn't allow direct I/O)" : "");
    else if (report->cached)
        g_string_append(text, "File system: (cached: it doesn't allow direct I/O)\n");
    g_autofree char *size = xd_bench_format_size(report->test_size);
    g_string_append_printf(text, "Test size: %s, best of %u run%s\n", size, report->runs,
                           report->runs == 1 ? "" : "s");
    g_string_append_printf(text, "Measure time: %u s, interval: %u s\n", report->measure_seconds,
                           report->interval_seconds);
    g_string_append_printf(text, "Test data: %s\n", report->random_data ? "Random" : "Zeros");
    g_string_append_printf(text, "Profile: %s\n\n", xd_bench_get_profile_name(report->profile));

    /* The table, with the first column as wide as its longest test. */
    long width = 4;
    char **names = g_new0(char *, report->row_count + 1);
    for (size_t i = 0; i < report->row_count; i++) {
        names[i] = xd_bench_describe_test(&report->tests[i]);
        width = XD_MAX(width, g_utf8_strlen(names[i], -1));
    }
    static constexpr long CELL_WIDTH = 16;
    append_padded(text, "Test", width, false);
    append_padded(text, "Read", CELL_WIDTH, true);
    append_padded(text, "Write", CELL_WIDTH, true);
    g_string_append_c(text, '\n');
    for (size_t i = 0; i < report->row_count; i++) {
        g_autofree char *read = format_cell(&report->read_results[i], report->unit);
        g_autofree char *write = format_cell(&report->write_results[i], report->unit);
        append_padded(text, names[i], width, false);
        append_padded(text, read, CELL_WIDTH, true);
        append_padded(text, write, CELL_WIDTH, true);
        g_string_append_c(text, '\n');
    }
    g_strfreev(names);

    /* Each result in every unit, as the cells' tooltips have them. */
    g_string_append(text, "\nDetails:\n");
    for (size_t i = 0; i < report->row_count; i++) {
        const xd_bench_measure *pairs[] = {&report->read_results[i], &report->write_results[i]};
        for (size_t k = 0; k < XD_ARRAY_LEN(pairs); k++) {
            if (!xd_bench_has_result(pairs[k]))
                continue;
            g_autofree char *name = xd_bench_describe_test(&report->tests[i]);
            g_string_append_printf(text, "%s, %s: %.2f MB/s, %.2f IOPS, %.2f μs\n", name,
                                   k == 0 ? "read" : "write",
                                   xd_bench_get_megabytes_per_second(pairs[k]),
                                   xd_bench_get_iops(pairs[k]),
                                   xd_bench_get_mean_latency_us(pairs[k]));
        }
    }
    return g_string_free(text, FALSE);
}

/* ---- health ------------------------------------------------------------------------ */

const char *xd_health_get_status_label(xd_health_status status) {
    switch (status) {
    case XD_HEALTH_GOOD:
        return "Good";
    case XD_HEALTH_CAUTION:
        return "Caution";
    case XD_HEALTH_BAD:
        return "Bad";
    case XD_HEALTH_UNKNOWN:
        break;
    }
    return "Unknown";
}

/* The attributes counting sectors the drive had to give up on or move:
 * 05 reallocated, C5 pending (waiting to be reallocated), C6
 * uncorrectable. */
static bool is_sector_attribute(uint8_t id) { return id == 0x05 || id == 0xC5 || id == 0xC6; }

xd_attribute_mark xd_health_get_attribute_mark(const xd_ata_attribute *attribute) {
    /* A threshold of 0 means the attribute never fails (it's there to be
     * read), so only a non-zero one counts. */
    if (attribute->threshold > 0 && attribute->current >= 0 &&
        attribute->current <= attribute->threshold)
        return XD_ATTRIBUTE_BAD;
    if (is_sector_attribute(attribute->id) && attribute->raw > 0)
        return XD_ATTRIBUTE_CAUTION;
    return XD_ATTRIBUTE_NORMAL;
}

static void set_reason(char **reason, char *text) {
    if (reason != nullptr)
        *reason = text;
    else
        g_free(text);
}

xd_health_status xd_health_get_ata_status(const xd_ata_health *health, char **reason) {
    if (!health->smart_supported) {
        set_reason(reason, g_strdup("The drive doesn't support S.M.A.R.T."));
        return XD_HEALTH_UNKNOWN;
    }
    if (!health->smart_enabled) {
        set_reason(reason, g_strdup("S.M.A.R.T. is turned off on the drive."));
        return XD_HEALTH_UNKNOWN;
    }
    if (health->smart_failing) {
        set_reason(reason, g_strdup("The drive reports that it is failing. Back up its data "
                                    "now and replace it."));
        return XD_HEALTH_BAD;
    }
    if (health->attribute_count == 0) {
        set_reason(reason, g_strdup("The drive hasn't reported its S.M.A.R.T. attributes."));
        return XD_HEALTH_UNKNOWN;
    }
    const xd_ata_attribute *caution = nullptr;
    for (size_t i = 0; i < health->attribute_count; i++) {
        const xd_ata_attribute *attribute = &health->attributes[i];
        xd_attribute_mark mark = xd_health_get_attribute_mark(attribute);
        if (mark == XD_ATTRIBUTE_BAD) {
            g_autofree char *name = xd_health_format_attribute_name(attribute->name);
            set_reason(reason, g_strdup_printf("%s (%02X) is at or under its threshold. Back up "
                                               "the drive's data now.",
                                               name, attribute->id));
            return XD_HEALTH_BAD;
        }
        if (mark == XD_ATTRIBUTE_CAUTION && caution == nullptr)
            caution = attribute;
    }
    if (caution != nullptr) {
        g_autofree char *name = xd_health_format_attribute_name(caution->name);
        set_reason(reason, g_strdup_printf("%s (%02X) is %" PRId64 ": the drive has sectors it "
                                           "couldn't read or had to move.",
                                           name, caution->id, caution->raw));
        return XD_HEALTH_CAUTION;
    }
    set_reason(reason, g_strdup("The drive reports no problems."));
    return XD_HEALTH_GOOD;
}

const char *xd_health_describe_critical_warning(const char *warning) {
    static const struct {
        const char *name, *text;
    } warnings[] = {
        {"spare", "the available spare is under its threshold"},
        {"temperature", "the temperature is outside its limits"},
        {"degraded", "its reliability is degraded by media errors"},
        {"readonly", "its media has been made read-only"},
        {"volatile_mem", "the volatile memory backup has failed"},
        {"pmr_readonly", "its persistent memory region has been made read-only"},
    };
    for (size_t i = 0; i < XD_ARRAY_LEN(warnings); i++)
        if (strcmp(warning, warnings[i].name) == 0)
            return warnings[i].text;
    return warning;
}

xd_health_status xd_health_get_nvme_status(const xd_nvme_health *health, char **reason) {
    if (!health->has_log) {
        set_reason(reason, g_strdup("The drive hasn't reported its health log."));
        return XD_HEALTH_UNKNOWN;
    }
    if (health->critical_warnings != nullptr && health->critical_warnings[0] != nullptr) {
        GString *text = g_string_new("The drive warns that ");
        for (size_t i = 0; health->critical_warnings[i] != nullptr; i++) {
            if (i > 0)
                g_string_append(text, health->critical_warnings[i + 1] == nullptr ? " and " : ", ");
            g_string_append(text, xd_health_describe_critical_warning(health->critical_warnings[i]));
        }
        g_string_append(text, ". Back up its data now.");
        set_reason(reason, g_string_free(text, FALSE));
        return XD_HEALTH_BAD;
    }
    if (health->available_spare >= 0 && health->spare_threshold > 0 &&
        health->available_spare < health->spare_threshold) {
        set_reason(reason, g_strdup_printf("The available spare (%d%%) is under its threshold "
                                           "(%d%%).",
                                           health->available_spare, health->spare_threshold));
        return XD_HEALTH_CAUTION;
    }
    if (health->percent_used >= 90) {
        set_reason(reason, g_strdup_printf("%d%% of the drive's rated life is used.",
                                           health->percent_used));
        return XD_HEALTH_CAUTION;
    }
    set_reason(reason, g_strdup("The drive reports no problems."));
    return XD_HEALTH_GOOD;
}

char *xd_health_format_attribute_name(const char *name) {
    if (name == nullptr || name[0] == '\0')
        return g_strdup("Unknown attribute");
    char *text = g_strdup(name);
    for (char *cursor = text; *cursor != '\0'; cursor++)
        if (*cursor == '-' || *cursor == '_')
            *cursor = ' ';
    text[0] = g_ascii_toupper(text[0]);
    return text;
}

char *xd_health_format_raw(int64_t raw, int raw_unit) {
    switch (raw_unit) {
    case 2: { /* milliseconds: hours, as drives count their age */
        int64_t hours = raw / (3600 * 1000);
        return g_strdup_printf("%" PRId64 " hour%s", hours, hours == 1 ? "" : "s");
    }
    case 3:
        return g_strdup_printf("%" PRId64 " sector%s", raw, raw == 1 ? "" : "s");
    case 4:
        return g_strdup_printf("%.0f °C", (double)raw / 1000.0 - 273.15);
    default:
        return g_strdup_printf("%" PRId64, raw);
    }
}

char *xd_health_format_data_size(uint64_t bytes) {
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < XD_ARRAY_LEN(units)) {
        value /= 1000.0;
        unit++;
    }
    if (unit == 0)
        return g_strdup_printf("%" PRIu64 " bytes", bytes);
    if (value >= 100.0)
        return g_strdup_printf("%.0f %s", value, units[unit]);
    if (value >= 10.0)
        return g_strdup_printf("%.1f %s", value, units[unit]);
    return g_strdup_printf("%.2f %s", value, units[unit]);
}

char *xd_health_format_kelvin(double kelvin) {
    if (kelvin <= 0)
        return g_strdup("");
    return g_strdup_printf("%.0f °C", kelvin - 273.15);
}
