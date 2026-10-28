/* SPDX-License-Identifier: MIT
 * Unit tests for xd-diskbench's arithmetic (block offsets, statistics,
 * units, Copy's text, the health rules) and the engine's clean-up of
 * leftover test files; included by test_main.c after its CHECK macro.
 */
#include "../src/diskbench/benchlib.h"
#include "../src/diskbench/engine.h"

static void test_diskbench_offsets(void) {
    /* Sequential: block after block from the slice's start, wrapping. */
    xd_bench_offsets offsets;
    xd_bench_init_offsets(&offsets, XD_BENCH_SEQUENTIAL, 4096, 2, 3, 1);
    CHECK(xd_bench_get_next_offset(&offsets) == 2 * 4096);
    CHECK(xd_bench_get_next_offset(&offsets) == 3 * 4096);
    CHECK(xd_bench_get_next_offset(&offsets) == 4 * 4096);
    CHECK(xd_bench_get_next_offset(&offsets) == 2 * 4096);

    /* Random: aligned, inside the slice, the same for the same seed, and
     * reaching every block of a small file. */
    xd_bench_offsets first, second;
    xd_bench_init_offsets(&first, XD_BENCH_RANDOM, 4096, 10, 16, 42);
    xd_bench_init_offsets(&second, XD_BENCH_RANDOM, 4096, 10, 16, 42);
    bool aligned = true, inside = true, repeatable = true;
    bool seen[16] = {};
    for (int i = 0; i < 1000; i++) {
        uint64_t offset = xd_bench_get_next_offset(&first);
        aligned = aligned && offset % 4096 == 0;
        inside = inside && offset >= 10 * 4096 && offset < 26 * 4096;
        repeatable = repeatable && offset == xd_bench_get_next_offset(&second);
        if (offset >= 10 * 4096 && offset < 26 * 4096)
            seen[offset / 4096 - 10] = true;
    }
    CHECK(aligned && inside && repeatable);
    bool all = true;
    for (size_t i = 0; i < XD_ARRAY_LEN(seen); i++)
        all = all && seen[i];
    CHECK(all);
    xd_bench_init_offsets(&second, XD_BENCH_RANDOM, 4096, 10, 16, 43);
    xd_bench_init_offsets(&first, XD_BENCH_RANDOM, 4096, 10, 16, 42);
    bool differs = false;
    for (int i = 0; i < 16; i++)
        differs = differs || xd_bench_get_next_offset(&first) != xd_bench_get_next_offset(&second);
    CHECK(differs);

    /* The mixed row's reads come out near 70 in 100. */
    uint64_t state = 7;
    int reads = 0;
    for (int i = 0; i < 10000; i++)
        reads += xd_bench_choose_read(&state, 70) ? 1 : 0;
    CHECK(reads > 6700 && reads < 7300);
}

static void test_diskbench_statistics(void) {
    /* 500 MB in 2 s over 1000 operations of 4 ms each. */
    xd_bench_measure measure = {.bytes = 500000000,
                                .operations = 1000,
                                .elapsed_ns = 2000000000,
                                .latency_ns = 4000000000};
    CHECK(fabs(xd_bench_get_megabytes_per_second(&measure) - 250.0) < 1e-9);
    CHECK(fabs(xd_bench_get_iops(&measure) - 500.0) < 1e-9);
    CHECK(fabs(xd_bench_get_mean_latency_us(&measure) - 4000.0) < 1e-9);
    xd_bench_measure none = {};
    CHECK(!xd_bench_has_result(&none) && xd_bench_get_megabytes_per_second(&none) == 0);
    xd_bench_measure faster = measure;
    faster.elapsed_ns = 1000000000;
    CHECK(xd_bench_is_better(&faster, &measure) && !xd_bench_is_better(&measure, &faster));
    CHECK(xd_bench_is_better(&measure, &none) && !xd_bench_is_better(&none, &measure));

    /* Units. */
    char *text = xd_bench_format_value(&measure, XD_BENCH_UNIT_MEGABYTES);
    CHECK(strcmp(text, "250.00") == 0);
    g_free(text);
    text = xd_bench_format_value(&measure, XD_BENCH_UNIT_GIGABYTES);
    CHECK(strcmp(text, "0.250") == 0);
    g_free(text);
    text = xd_bench_format_value(&measure, XD_BENCH_UNIT_IOPS);
    CHECK(strcmp(text, "500.00") == 0);
    g_free(text);
    text = xd_bench_format_value(&measure, XD_BENCH_UNIT_MICROSECONDS);
    CHECK(strcmp(text, "4000.00") == 0);
    g_free(text);
    text = xd_bench_format_value(&none, XD_BENCH_UNIT_MEGABYTES);
    CHECK(strcmp(text, "") == 0);
    g_free(text);
    CHECK(strcmp(xd_bench_get_unit_label(XD_BENCH_UNIT_MICROSECONDS), "μs") == 0);
    text = xd_bench_format_details(&measure);
    CHECK(strcmp(text, "250.00 MB/s\n500.00 IOPS\n4000.00 μs average latency") == 0);
    g_free(text);

    text = xd_bench_format_size(16u * 1024 * 1024);
    CHECK(strcmp(text, "16 MiB") == 0);
    g_free(text);
    text = xd_bench_format_size((uint64_t)64 << 30);
    CHECK(strcmp(text, "64 GiB") == 0);
    g_free(text);
    text = xd_bench_format_size(1536);
    CHECK(strcmp(text, "1.5 KiB") == 0);
    g_free(text);
    text = xd_bench_format_size(512);
    CHECK(strcmp(text, "512 bytes") == 0);
    g_free(text);
    CHECK(xd_bench_test_sizes[0] == 16u * 1024 * 1024 &&
          xd_bench_test_sizes[xd_bench_test_size_count - 1] == (uint64_t)64 << 30);
}

static void test_diskbench_profiles(void) {
    xd_bench_test tests[XD_BENCH_MAX_ROWS];
    size_t count = xd_bench_get_profile_tests(XD_BENCH_PROFILE_DEFAULT, tests);
    CHECK(count == 4);
    CHECK(tests[0].pattern == XD_BENCH_SEQUENTIAL && tests[0].block_size == 1024 * 1024 &&
          tests[0].queue_depth == 8 && tests[0].threads == 1);
    CHECK(tests[3].pattern == XD_BENCH_RANDOM && tests[3].block_size == 4096 &&
          tests[3].queue_depth == 1);
    char *text = xd_bench_describe_test(&tests[0]);
    CHECK(strcmp(text, "Sequential, 1 MiB blocks, queue depth 8, 1 thread") == 0);
    g_free(text);
    text = xd_bench_describe_test(&tests[2]);
    CHECK(strcmp(text, "Random, 4 KiB blocks, queue depth 32, 1 thread") == 0);
    g_free(text);
    char *title = nullptr, *subtitle = nullptr;
    xd_bench_get_test_title(&tests[2], &title, &subtitle);
    CHECK(strcmp(title, "Random 4 KiB") == 0 && strcmp(subtitle, "Queue depth 32, 1 thread") == 0);
    g_free(title);
    g_free(subtitle);
    for (int profile = 0; profile < XD_BENCH_PROFILE_COUNT; profile++) {
        count = xd_bench_get_profile_tests((xd_bench_profile)profile, tests);
        bool valid = count > 0 && count <= XD_BENCH_MAX_ROWS;
        for (size_t i = 0; i < count; i++)
            valid = valid && xd_bench_is_test_valid(&tests[i]);
        CHECK(valid);
    }
    count = xd_bench_get_profile_tests(XD_BENCH_PROFILE_REALISTIC, tests);
    CHECK(count == 5 && tests[4].read_percent == 70);
    text = xd_bench_describe_test(&tests[4]);
    CHECK(strcmp(text, "Mixed 70/30 read/write, random, 4 KiB blocks, queue depth 1, 1 thread") ==
          0);
    g_free(text);
    xd_bench_test bad = {XD_BENCH_RANDOM, 3000, 1, 1, 0};
    CHECK(!xd_bench_is_test_valid(&bad));
    bad = (xd_bench_test){XD_BENCH_RANDOM, 4096, 0, 1, 0};
    CHECK(!xd_bench_is_test_valid(&bad));
}

static void test_diskbench_report(void) {
    xd_bench_test tests[XD_BENCH_MAX_ROWS];
    size_t count = xd_bench_get_profile_tests(XD_BENCH_PROFILE_DEFAULT, tests);
    xd_bench_measure reads[XD_BENCH_MAX_ROWS] = {}, writes[XD_BENCH_MAX_ROWS] = {};
    reads[0] = (xd_bench_measure){1000000000, 1000, 1000000000, 2000000000};
    writes[0] = (xd_bench_measure){500000000, 500, 1000000000, 1000000000};
    xd_bench_report report = {
        .drive = "scratch (/home/me/scratch), 12.3 GB free",
        .filesystem = "tmpfs",
        .date = "2026-10-27 12:00",
        .test_size = 16u * 1024 * 1024,
        .runs = 1,
        .measure_seconds = 5,
        .interval_seconds = 0,
        .random_data = true,
        .cached = true,
        .profile = XD_BENCH_PROFILE_DEFAULT,
        .unit = XD_BENCH_UNIT_MEGABYTES,
        .row_count = count,
        .tests = tests,
        .read_results = reads,
        .write_results = writes,
    };
    char *text = xd_bench_format_report(&report);
    const char *expected =
        "Disk Benchmark and Health: benchmark results\n"
        "Date: 2026-10-27 12:00\n"
        "Drive: scratch (/home/me/scratch), 12.3 GB free\n"
        "File system: tmpfs (cached: it doesn't allow direct I/O)\n"
        "Test size: 16 MiB, best of 1 run\n"
        "Measure time: 5 s, interval: 0 s\n"
        "Test data: Random\n"
        "Profile: Default\n"
        "\n"
        "Test                                                         Read           Write\n"
        "Sequential, 1 MiB blocks, queue depth 8, 1 thread    1000.00 MB/s     500.00 MB/s\n"
        "Sequential, 1 MiB blocks, queue depth 1, 1 thread               -               -\n"
        "Random, 4 KiB blocks, queue depth 32, 1 thread                  -               -\n"
        "Random, 4 KiB blocks, queue depth 1, 1 thread                   -               -\n"
        "\n"
        "Details:\n"
        "Sequential, 1 MiB blocks, queue depth 8, 1 thread, read: 1000.00 MB/s, 1000.00 IOPS, "
        "2000.00 μs\n"
        "Sequential, 1 MiB blocks, queue depth 8, 1 thread, write: 500.00 MB/s, 500.00 IOPS, "
        "2000.00 μs\n";
    CHECK(strcmp(text, expected) == 0);
    if (strcmp(text, expected) != 0)
        fprintf(stderr, "%s", text);
    g_free(text);
}

static void test_diskbench_health(void) {
    /* ATA: a healthy drive, one with pending sectors, one under a
     * threshold, one that says it's failing, one without S.M.A.R.T. */
    xd_ata_attribute good[] = {
        {0x01, "raw-read-error-rate", 100, 100, 6, 0, 1},
        {0x05, "reallocated-sector-count", 100, 100, 36, 0, 3},
        {0xC2, "temperature-celsius-2", 35, 45, 0, 308150, 4},
        {0xC5, "current-pending-sector", 100, 100, 0, 0, 3},
    };
    xd_ata_health health = {true, true, false, good, XD_ARRAY_LEN(good)};
    char *reason = nullptr;
    CHECK(xd_health_get_ata_status(&health, &reason) == XD_HEALTH_GOOD);
    g_free(reason);
    CHECK(xd_health_get_attribute_mark(&good[2]) == XD_ATTRIBUTE_NORMAL); /* threshold 0 */

    xd_ata_attribute pending[XD_ARRAY_LEN(good)];
    memcpy(pending, good, sizeof(good));
    pending[3].raw = 8;
    health.attributes = pending;
    CHECK(xd_health_get_ata_status(&health, &reason) == XD_HEALTH_CAUTION);
    CHECK(strstr(reason, "C5") != nullptr);
    g_free(reason);
    CHECK(xd_health_get_attribute_mark(&pending[3]) == XD_ATTRIBUTE_CAUTION);

    xd_ata_attribute worn[XD_ARRAY_LEN(good)];
    memcpy(worn, pending, sizeof(good));
    worn[1].current = 36; /* at its threshold */
    health.attributes = worn;
    CHECK(xd_health_get_ata_status(&health, nullptr) == XD_HEALTH_BAD);
    CHECK(xd_health_get_attribute_mark(&worn[1]) == XD_ATTRIBUTE_BAD);

    health.attributes = good;
    health.smart_failing = true;
    CHECK(xd_health_get_ata_status(&health, nullptr) == XD_HEALTH_BAD);
    health.smart_failing = false;
    health.smart_supported = false;
    CHECK(xd_health_get_ata_status(&health, &reason) == XD_HEALTH_UNKNOWN);
    g_free(reason);
    health.smart_supported = true;
    health.attribute_count = 0;
    CHECK(xd_health_get_ata_status(&health, nullptr) == XD_HEALTH_UNKNOWN);

    /* NVMe. */
    xd_nvme_health nvme = {true, nullptr, 100, 10, 3};
    CHECK(xd_health_get_nvme_status(&nvme, nullptr) == XD_HEALTH_GOOD);
    nvme.percent_used = 90;
    CHECK(xd_health_get_nvme_status(&nvme, nullptr) == XD_HEALTH_CAUTION);
    nvme.percent_used = 3;
    nvme.available_spare = 5;
    CHECK(xd_health_get_nvme_status(&nvme, nullptr) == XD_HEALTH_CAUTION);
    const char *const warnings[] = {"spare", "temperature", nullptr};
    nvme.critical_warnings = warnings;
    CHECK(xd_health_get_nvme_status(&nvme, &reason) == XD_HEALTH_BAD);
    CHECK(strcmp(reason, "The drive warns that the available spare is under its threshold and "
                         "the temperature is outside its limits. Back up its data now.") == 0);
    g_free(reason);
    nvme.has_log = false;
    CHECK(xd_health_get_nvme_status(&nvme, nullptr) == XD_HEALTH_UNKNOWN);
    CHECK(strcmp(xd_health_get_status_label(XD_HEALTH_CAUTION), "Caution") == 0);

    /* Formatting. */
    char *text = xd_health_format_attribute_name("reallocated-sector-count");
    CHECK(strcmp(text, "Reallocated sector count") == 0);
    g_free(text);
    text = xd_health_format_attribute_name("Current_Pending_Sector");
    CHECK(strcmp(text, "Current Pending Sector") == 0);
    g_free(text);
    text = xd_health_format_raw(308150, 4);
    CHECK(strcmp(text, "35 °C") == 0);
    g_free(text);
    text = xd_health_format_raw((int64_t)1234 * 3600 * 1000, 2);
    CHECK(strcmp(text, "1234 hours") == 0);
    g_free(text);
    text = xd_health_format_raw(1, 3);
    CHECK(strcmp(text, "1 sector") == 0);
    g_free(text);
    text = xd_health_format_data_size(1240000000000);
    CHECK(strcmp(text, "1.24 TB") == 0);
    g_free(text);
    text = xd_health_format_data_size(68719476736);
    CHECK(strcmp(text, "68.7 GB") == 0);
    g_free(text);
    text = xd_health_format_kelvin(0);
    CHECK(strcmp(text, "") == 0);
    g_free(text);
}

/* Leftover test files go only when the program that made them has gone. */
static void test_diskbench_leftovers(void) {
    char *root = g_dir_make_tmp("xd-diskbench-XXXXXX", nullptr);
    char *work = xd_bench_get_work_directory(root);
    CHECK(g_str_has_suffix(work, "/.xd-diskbench"));
    g_mkdir(work, 0700);
    char *gone = g_strdup_printf("%s/xd-diskbench-%d-0badf00d.tmp", work, INT32_MAX);
    char *mine = g_strdup_printf("%s/xd-diskbench-%d-12345678.tmp", work, (int)getpid());
    char *alive = g_strdup_printf("%s/xd-diskbench-1-12345678.tmp", work); /* init */
    char *other = g_strdup_printf("%s/notes.txt", work);
    const char *const files[] = {gone, mine, alive, other};
    for (size_t i = 0; i < XD_ARRAY_LEN(files); i++)
        g_file_set_contents(files[i], "", 0, nullptr);
    CHECK(xd_bench_remove_leftovers(work) == 1);
    CHECK(!g_file_test(gone, G_FILE_TEST_EXISTS));
    CHECK(g_file_test(mine, G_FILE_TEST_EXISTS) && g_file_test(alive, G_FILE_TEST_EXISTS) &&
          g_file_test(other, G_FILE_TEST_EXISTS));
    CHECK(xd_bench_remove_leftovers(work) == 0);
    for (size_t i = 0; i < XD_ARRAY_LEN(files); i++)
        g_unlink(files[i]);
    g_rmdir(work);
    g_rmdir(root);
    g_free(gone);
    g_free(mine);
    g_free(alive);
    g_free(other);
    g_free(work);
    g_free(root);
}

static void test_diskbench(void) {
    test_diskbench_offsets();
    test_diskbench_statistics();
    test_diskbench_profiles();
    test_diskbench_report();
    test_diskbench_health();
    test_diskbench_leftovers();
}
