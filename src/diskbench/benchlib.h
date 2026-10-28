/* SPDX-License-Identifier: MIT
 * xd-diskbench's arithmetic, without GTK or I/O so it can be tested on its
 * own: the tests each profile runs, the block offsets they visit, the
 * statistics from what was measured, the units results are shown in, the
 * text Copy puts on the clipboard, and the health rules for ATA and NVMe
 * drives.
 */
#ifndef XD_DISKBENCH_BENCHLIB_H
#define XD_DISKBENCH_BENCHLIB_H

#include <glib.h>
#include <stdint.h>

/* ---- the tests ---------------------------------------------------------------------- */

typedef enum xd_bench_pattern : uint8_t {
    XD_BENCH_SEQUENTIAL,
    XD_BENCH_RANDOM,
} xd_bench_pattern;

/* One row of the Benchmark tab. A row with read_percent 0 is measured
 * twice, reading and writing; a mixed row (read_percent 1..99) once,
 * each operation a read or a write by chance. */
typedef struct xd_bench_test {
    xd_bench_pattern pattern;
    uint32_t block_size;
    uint32_t queue_depth;
    uint32_t threads;
    uint8_t read_percent;
} xd_bench_test;

#define XD_BENCH_MAX_ROWS 5
#define XD_BENCH_MAX_QUEUE_DEPTH 512u
#define XD_BENCH_MAX_THREADS 64u

typedef enum xd_bench_profile : uint8_t {
    XD_BENCH_PROFILE_DEFAULT,
    XD_BENCH_PROFILE_PEAK,
    XD_BENCH_PROFILE_REALISTIC,
    XD_BENCH_PROFILE_COUNT,
} xd_bench_profile;

/* "Default", "Peak performance", "Realistic". */
const char *xd_bench_get_profile_name(xd_bench_profile profile);
/* The profile's rows into `tests`; returns how many. */
size_t xd_bench_get_profile_tests(xd_bench_profile profile, xd_bench_test tests[XD_BENCH_MAX_ROWS]);

/* "Sequential, 1 MiB blocks, queue depth 8, 1 thread"; a mixed row
 * starts "Mixed 70/30 read/write, random, ...". Free with g_free. */
char *xd_bench_describe_test(const xd_bench_test *test);
/* The row button's two lines: "Sequential 1 MiB" and "Queue depth 8, 1
 * thread". Free both with g_free. */
void xd_bench_get_test_title(const xd_bench_test *test, char **title, char **subtitle);
/* Whether the engine can run it: block size a power of two from 512
 * bytes to 64 MiB, queue depth and threads within their limits. */
bool xd_bench_is_test_valid(const xd_bench_test *test);

/* The Test size list: 16 MiB to 64 GiB. */
extern const uint64_t xd_bench_test_sizes[];
extern const size_t xd_bench_test_size_count;
#define XD_BENCH_DEFAULT_TEST_SIZE ((uint64_t)1 << 30)

/* The block sizes the Settings dialog offers: 4 KiB to 8 MiB. */
extern const uint32_t xd_bench_block_sizes[];
extern const size_t xd_bench_block_size_count;

/* Binary sizes, exactly when they're whole: "512 bytes", "4 KiB",
 * "1 MiB", "1 GiB", else one decimal ("1.5 GiB"). Free with g_free. */
char *xd_bench_format_size(uint64_t bytes);

/* ---- offsets ----------------------------------------------------------------------- */

/* Where each operation goes: blocks [first_block, first_block +
 * block_count) of the test file, in order (wrapping round) or at random,
 * aligned to the block size. The random sequence depends only on the
 * seed, so a run can be repeated. */
typedef struct xd_bench_offsets {
    xd_bench_pattern pattern;
    uint64_t block_size, first_block, block_count, position, random_state;
} xd_bench_offsets;

void xd_bench_init_offsets(xd_bench_offsets *offsets, xd_bench_pattern pattern,
                           uint64_t block_size, uint64_t first_block, uint64_t block_count,
                           uint64_t seed);
/* The next operation's byte offset. */
uint64_t xd_bench_get_next_offset(xd_bench_offsets *offsets);

/* A fast seeded generator (splitmix64), for offsets, test data and the
 * mixed row's choice of read or write. */
uint64_t xd_bench_get_next_random(uint64_t *state);
/* For a mixed row: whether the next operation reads, read_percent times
 * in a hundred. */
bool xd_bench_choose_read(uint64_t *state, unsigned read_percent);

/* ---- statistics -------------------------------------------------------------------- */

/* What one measured run (or the best of several) moved, in how long. */
typedef struct xd_bench_measure {
    uint64_t bytes;
    uint64_t operations;
    uint64_t elapsed_ns;
    uint64_t latency_ns; /* summed over the operations */
} xd_bench_measure;

/* MB/s (1 MB = 1,000,000 bytes, as drives are sold), IOPS, and the mean
 * latency in microseconds; 0 when nothing was measured. */
double xd_bench_get_megabytes_per_second(const xd_bench_measure *measure);
double xd_bench_get_iops(const xd_bench_measure *measure);
double xd_bench_get_mean_latency_us(const xd_bench_measure *measure);
/* Whether a run measured anything. */
bool xd_bench_has_result(const xd_bench_measure *measure);
/* Whether `candidate` beats `best` (more bytes a second), so the best of
 * several runs is reported. */
bool xd_bench_is_better(const xd_bench_measure *candidate, const xd_bench_measure *best);

typedef enum xd_bench_unit : uint8_t {
    XD_BENCH_UNIT_MEGABYTES,
    XD_BENCH_UNIT_GIGABYTES,
    XD_BENCH_UNIT_IOPS,
    XD_BENCH_UNIT_MICROSECONDS,
    XD_BENCH_UNIT_COUNT,
} xd_bench_unit;

/* "MB/s", "GB/s", "IOPS", "μs". */
const char *xd_bench_get_unit_label(xd_bench_unit unit);
/* The result in `unit`: "1234.56" (MB/s, IOPS, μs) or "1.235" (GB/s);
 * "" when nothing was measured. Free with g_free. */
char *xd_bench_format_value(const xd_bench_measure *measure, xd_bench_unit unit);
/* All three at once, for a tooltip: "1234.56 MB/s\n1177.38 IOPS\n6794.52 μs
 * average latency". Free with g_free. */
char *xd_bench_format_details(const xd_bench_measure *measure);

/* ---- Copy's text -------------------------------------------------------------------- */

typedef struct xd_bench_report {
    const char *drive;      /* "Documents (/home/me/Documents), 12.3 GB free" */
    const char *filesystem; /* "ext4", or "" */
    const char *date;       /* "2026-10-27 12:00" */
    uint64_t test_size;
    unsigned runs, measure_seconds, interval_seconds;
    bool random_data, cached;
    xd_bench_profile profile;
    xd_bench_unit unit;
    size_t row_count;
    const xd_bench_test *tests;
    const xd_bench_measure *read_results;  /* row_count of them */
    const xd_bench_measure *write_results; /* row_count of them */
} xd_bench_report;

/* The results as plain text, with what they were measured on and how.
 * Free with g_free. */
char *xd_bench_format_report(const xd_bench_report *report);

/* ---- health ------------------------------------------------------------------------ */

typedef enum xd_health_status : uint8_t {
    XD_HEALTH_UNKNOWN,
    XD_HEALTH_GOOD,
    XD_HEALTH_CAUTION,
    XD_HEALTH_BAD,
} xd_health_status;

/* "Unknown", "Good", "Caution", "Bad". */
const char *xd_health_get_status_label(xd_health_status status);

/* One row of SmartGetAttributes on Drive.Ata. UDisks2 gives the raw value
 * already interpreted ("pretty"), in raw_unit: 0 unknown, 1 a count, 2
 * milliseconds, 3 sectors, 4 millikelvin. current, worst and threshold
 * are -1 when the drive has none. */
typedef struct xd_ata_attribute {
    uint8_t id;
    const char *name;
    int current, worst, threshold;
    int64_t raw;
    int raw_unit;
} xd_ata_attribute;

typedef enum xd_attribute_mark : uint8_t {
    XD_ATTRIBUTE_NORMAL,
    XD_ATTRIBUTE_CAUTION, /* reallocated, pending or uncorrectable sectors */
    XD_ATTRIBUTE_BAD,     /* at or under its (non-zero) threshold */
} xd_attribute_mark;

xd_attribute_mark xd_health_get_attribute_mark(const xd_ata_attribute *attribute);

typedef struct xd_ata_health {
    bool smart_supported, smart_enabled, smart_failing;
    const xd_ata_attribute *attributes;
    size_t attribute_count;
} xd_ata_health;

/* Bad: the drive says it's failing, or an attribute is at or under its
 * threshold. Caution: reallocated, pending or uncorrectable sectors.
 * Unknown: no S.M.A.R.T. data. `reason`, if not nullptr, gets a sentence
 * saying why (g_free it). */
xd_health_status xd_health_get_ata_status(const xd_ata_health *health, char **reason);

typedef struct xd_nvme_health {
    bool has_log;
    const char *const *critical_warnings; /* SmartCriticalWarning, nullptr-terminated */
    int available_spare, spare_threshold, percent_used; /* -1 when unknown */
} xd_nvme_health;

/* Bad: any critical warning. Caution: available spare under its
 * threshold, or 90% or more of the rated life used. */
xd_health_status xd_health_get_nvme_status(const xd_nvme_health *health, char **reason);

/* A critical warning's words: "spare" -> "the available spare is under its
 * threshold", ...; the name itself when it's a new one. */
const char *xd_health_describe_critical_warning(const char *warning);

/* UDisks2's attribute names, readable: "reallocated-sector-count" and
 * "Reallocated_Sector_Ct" -> "Reallocated sector count" / "Reallocated
 * Sector Ct". Free with g_free. */
char *xd_health_format_attribute_name(const char *name);
/* An attribute's raw value in its unit: "8 sectors", "35 °C", "1234
 * hours", "17". Free with g_free. */
char *xd_health_format_raw(int64_t raw, int raw_unit);
/* Data in the units drives are sold in: "512 MB", "12.3 GB", "1.24 TB".
 * Free with g_free. */
char *xd_health_format_data_size(uint64_t bytes);
/* A Kelvin temperature as "35 °C"; "" when it's 0 (unknown). Free with
 * g_free. */
char *xd_health_format_kelvin(double kelvin);

#endif
