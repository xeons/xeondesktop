/* SPDX-License-Identifier: MIT
 * xd-thumbnailer: makes a file's freedesktop thumbnail, decoding the file
 * only inside a sandbox (sandbox.h). xd-files runs it for thumbnails the
 * cache doesn't have yet.
 *
 *   xd-thumbnailer [--size=normal|large|x-large|xx-large] FILE|URI
 *
 * prints the thumbnail's path and exits 0; exits 1 when the file can't be
 * thumbnailed (leaving a failure marker when decoding it failed), 2 when
 * the sandbox can't run.
 *
 * Inside the sandbox it runs again, as
 *   xd-thumbnailer --decode=SIZE -- INPUT [THUMBNAILER ARGUMENTS...]
 * and writes "XDT1" at once (the sandbox works), then the decoded image's
 * width and height (32-bit, native order) and its RGBA rows. The PNG is
 * written out here from those pixels, so nothing the sandbox sends is
 * parsed as an image format outside it.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sandbox.h"
#include "thumbcache.h"

static const char MAGIC[4] = {'X', 'D', 'T', '1'};
/* Wall-clock limit for one file (the sandbox also has a CPU-time limit):
 * a decoder that blocks instead of spinning. */
static constexpr int TIMEOUT_SECONDS = 30;

enum { EXIT_OK = 0, EXIT_CANT = 1, EXIT_BROKEN = 2, EXIT_DECODE_FAILED = 3 };

/* ---- inside the sandbox ------------------------------------------------------- */

static gboolean write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return FALSE;
        p += n;
        len -= (size_t)n;
    }
    return TRUE;
}

/* Fits `pb` in px x px (never larger than it was); takes `pb`. */
static GdkPixbuf *fit(GdkPixbuf *pb, int px) {
    int w = gdk_pixbuf_get_width(pb);
    int h = gdk_pixbuf_get_height(pb);
    if (w <= px && h <= px)
        return pb;
    double k = (double)px / MAX(w, h);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, MAX(1, (int)(w * k + 0.5)),
                                                MAX(1, (int)(h * k + 0.5)), GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    return scaled;
}

/* Decodes rest[0] (or runs the thumbnailer in rest[1...]) and writes the
 * picture's size and pixels to `out`. */
static int decode_to(int out, XdThumbSize size, char **rest) {
    const char *input = rest[0];
    if (rest[1] != nullptr) {
        /* An installed thumbnailer, run in here with the same limits: it
         * writes a PNG, which is decoded here like any image. */
        int status = 0;
        if (!g_spawn_sync("/tmp", &rest[1], nullptr,
                          G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, nullptr, nullptr,
                          nullptr, nullptr, &status, nullptr) ||
            !g_spawn_check_wait_status(status, nullptr))
            return EXIT_DECODE_FAILED;
        input = XD_SANDBOX_OUT;
    }
    int px = xd_thumb_size_px(size);
    int w = 0;
    int h = 0;
    if (gdk_pixbuf_get_file_info(input, &w, &h) == nullptr)
        return EXIT_DECODE_FAILED;
    /* Decoded straight to the size when larger, so a huge image needn't
     * fit in memory whole; a smaller one is kept as it is (the spec). */
    GdkPixbuf *pb = w > px || h > px ? gdk_pixbuf_new_from_file_at_scale(input, px, px, TRUE, nullptr)
                                     : gdk_pixbuf_new_from_file(input, nullptr);
    if (pb == nullptr)
        return EXIT_DECODE_FAILED;
    GdkPixbuf *upright = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    pb = fit(upright, px);
    GdkPixbuf *rgba = gdk_pixbuf_add_alpha(pb, FALSE, 0, 0, 0);
    g_object_unref(pb);
    if (rgba == nullptr)
        return EXIT_DECODE_FAILED;
    guint32 dims[2] = {(guint32)gdk_pixbuf_get_width(rgba), (guint32)gdk_pixbuf_get_height(rgba)};
    gboolean ok = write_all(out, dims, sizeof(dims));
    const guchar *pixels = gdk_pixbuf_read_pixels(rgba);
    size_t stride = (size_t)gdk_pixbuf_get_rowstride(rgba);
    for (guint32 y = 0; ok && y < dims[1]; y++)
        ok = write_all(out, pixels + y * stride, (size_t)dims[0] * 4);
    g_object_unref(rgba);
    return ok ? EXIT_OK : EXIT_DECODE_FAILED;
}

/* GCC's analyzer takes dup2()'s result for a new descriptor to close, but
 * it's stdout, which stays pointed at stderr until the process exits. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
static int decode(XdThumbSize size, char **rest) {
    if (rest == nullptr || rest[0] == nullptr)
        return EXIT_DECODE_FAILED;
    /* stdout carries the pixels; anything a library prints goes to
     * stderr instead. */
    int out = dup(STDOUT_FILENO);
    if (out < 0)
        return EXIT_DECODE_FAILED;
    int status = EXIT_DECODE_FAILED;
    if (dup2(STDERR_FILENO, STDOUT_FILENO) == STDOUT_FILENO && write_all(out, MAGIC, sizeof(MAGIC)))
        status = decode_to(out, size, rest);
    close(out);
    return status;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* ---- outside ---------------------------------------------------------------------- */

/* Kills the sandbox when it takes too long; its reads then end. */
typedef struct watchdog {
    GMutex lock;
    GCond cond;
    gboolean done;
    GSubprocess *proc;
} watchdog;

static gpointer watch(gpointer data) {
    watchdog *w = data;
    gint64 end = g_get_monotonic_time() + TIMEOUT_SECONDS * G_TIME_SPAN_SECOND;
    g_mutex_lock(&w->lock);
    while (!w->done) {
        if (!g_cond_wait_until(&w->cond, &w->lock, end)) {
            g_subprocess_force_exit(w->proc);
            break;
        }
    }
    g_mutex_unlock(&w->lock);
    return nullptr;
}

/* A .thumbnailer's Exec line as arguments, with %u, %i, %o and %s
 * replaced by the input's URI and path inside the sandbox, the output
 * path and the size. */
static char **thumbnailer_args(const char *exec, const char *inside, int px) {
    int argc = 0;
    char **argv = nullptr;
    if (!g_shell_parse_argv(exec, &argc, &argv, nullptr))
        return nullptr;
    char *uri = g_filename_to_uri(inside, nullptr, nullptr);
    for (int i = 0; i < argc; i++) {
        GString *arg = g_string_new(nullptr);
        for (const char *c = argv[i]; *c != '\0'; c++) {
            if (*c != '%' || c[1] == '\0') {
                g_string_append_c(arg, *c);
                continue;
            }
            c++;
            switch (*c) {
            case 'u': g_string_append(arg, uri != nullptr ? uri : ""); break;
            case 'i': g_string_append(arg, inside); break;
            case 'o': g_string_append(arg, XD_SANDBOX_OUT); break;
            case 's': g_string_append_printf(arg, "%d", px); break;
            case '%': g_string_append_c(arg, '%'); break;
            default:
                g_string_append_c(arg, '%');
                g_string_append_c(arg, *c);
                break;
            }
        }
        g_free(argv[i]);
        argv[i] = g_string_free(arg, FALSE);
    }
    g_free(uri);
    return argv;
}

typedef enum { RUN_OK, RUN_FAILED, RUN_BROKEN } run_result;

static gboolean read_exact(GInputStream *in, void *buf, gsize len) {
    gsize got = 0;
    return g_input_stream_read_all(in, buf, len, &got, nullptr, nullptr) && got == len;
}

/* Decodes `path` in the sandbox (with a thumbnailer when `exec`) into
 * `*out`. RUN_FAILED: the file couldn't be; RUN_BROKEN: the sandbox
 * didn't start. */
static run_result run_sandbox(const char *path, XdThumbSize size, const char *exec,
                              gboolean verbose, GdkPixbuf **out) {
    int px = xd_thumb_size_px(size);
    char *inside = xd_sandbox_input_path(path);
    GStrvBuilder *b = g_strv_builder_new();
    g_strv_builder_take(b, g_strconcat("--decode=", xd_thumb_size_name(size), nullptr));
    g_strv_builder_add(b, "--");
    g_strv_builder_add(b, inside);
    char **cmd = exec != nullptr ? thumbnailer_args(exec, inside, px) : nullptr;
    gboolean bad_exec = exec != nullptr && cmd == nullptr;
    for (char **c = cmd; c != nullptr && *c != nullptr; c++)
        g_strv_builder_add(b, *c);
    GStrv args = g_strv_builder_unref_to_strv(b);
    g_strfreev(cmd);
    g_free(inside);
    if (bad_exec) {
        g_strfreev(args);
        return RUN_FAILED; /* an Exec line that doesn't parse */
    }

    GError *err = nullptr;
    GSubprocess *proc = xd_sandbox_spawn(path, (const char *const *)args, verbose, &err);
    g_strfreev(args);
    if (proc == nullptr) {
        fprintf(stderr, "xd-thumbnailer: %s\n", err->message);
        g_error_free(err);
        return RUN_BROKEN;
    }
    watchdog w = {.proc = proc};
    g_mutex_init(&w.lock);
    g_cond_init(&w.cond);
    GThread *thread = g_thread_new("watchdog", watch, &w);

    GInputStream *in = g_subprocess_get_stdout_pipe(proc);
    run_result result = RUN_FAILED;
    char magic[sizeof(MAGIC)];
    guint32 dims[2] = {0, 0};
    guchar *pixels = nullptr;
    gsize len = 0;
    if (!read_exact(in, magic, sizeof(magic)) || memcmp(magic, MAGIC, sizeof(MAGIC)) != 0) {
        result = RUN_BROKEN;
    } else if (read_exact(in, dims, sizeof(dims)) && dims[0] >= 1 && dims[1] >= 1 &&
               dims[0] <= (guint32)px && dims[1] <= (guint32)px) {
        len = (gsize)dims[0] * dims[1] * 4;
        pixels = g_malloc(len);
        if (read_exact(in, pixels, len))
            result = RUN_OK;
    }
    /* Done once it has exited, and exited cleanly. */
    g_input_stream_close(in, nullptr, nullptr);
    g_subprocess_wait(proc, nullptr, nullptr);
    if (result == RUN_OK && !g_subprocess_get_successful(proc))
        result = RUN_FAILED;
    g_mutex_lock(&w.lock);
    w.done = TRUE;
    g_cond_signal(&w.cond);
    g_mutex_unlock(&w.lock);
    g_thread_join(thread);
    g_mutex_clear(&w.lock);
    g_cond_clear(&w.cond);
    if (result == RUN_BROKEN)
        fprintf(stderr, "xd-thumbnailer: the sandbox didn't start (is bwrap installed?)\n");
    g_object_unref(proc);

    if (result == RUN_OK) {
        GBytes *bytes = g_bytes_new_take(pixels, len);
        *out = gdk_pixbuf_new_from_bytes(bytes, GDK_COLORSPACE_RGB, TRUE, 8, (int)dims[0],
                                         (int)dims[1], (int)dims[0] * 4);
        g_bytes_unref(bytes);
    } else {
        g_free(pixels);
    }
    return result;
}

/* Makes the thumbnail of the regular file at `path`. */
static int make(const char *path, const char *uri, GFileInfo *info, const char *root,
                XdThumbSize size, gboolean verbose) {
    gint64 mtime = (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    gint64 bytes = g_file_info_get_size(info);
    const char *mime = g_file_info_get_content_type(info);
    char *dest = xd_thumb_path(root, uri, size);
    int status = EXIT_CANT;
    if (xd_thumb_is_current(dest, uri, mtime)) {
        printf("%s\n", dest);
        status = EXIT_OK;
    } else if (!xd_thumb_has_failed(root, uri, mtime) && xd_thumb_supported(mime)) {
        const char *exec = xd_thumb_pixbuf_reads(mime) ? nullptr : xd_thumbnailer_exec(mime);
        GdkPixbuf *pb = nullptr;
        GError *err = nullptr;
        switch (run_sandbox(path, size, exec, verbose, &pb)) {
        case RUN_OK:
            if (xd_thumb_save(pb, dest, uri, mtime, bytes, mime, &err)) {
                printf("%s\n", dest);
                status = EXIT_OK;
            }
            break;
        case RUN_FAILED:
            if (verbose)
                fprintf(stderr, "xd-thumbnailer: %s: couldn't be decoded\n", path);
            xd_thumb_mark_failed(root, uri, mtime, &err);
            break;
        case RUN_BROKEN:
            status = EXIT_BROKEN;
            break;
        }
        if (err != nullptr) {
            fprintf(stderr, "xd-thumbnailer: %s\n", err->message);
            g_error_free(err);
        }
        g_clear_object(&pb);
    } else if (verbose) {
        fprintf(stderr, "xd-thumbnailer: %s: failed before, or no thumbnailer for %s\n", path,
                mime != nullptr ? mime : "its type");
    }
    g_free(dest);
    return status;
}

static int thumbnail(const char *arg, XdThumbSize size, gboolean verbose) {
    GFile *file = g_file_new_for_commandline_arg(arg);
    char *path = g_file_get_path(file);
    char *uri = g_file_get_uri(file);
    GFileInfo *info =
        path != nullptr
            ? g_file_query_info(file,
                                G_FILE_ATTRIBUTE_STANDARD_TYPE
                                "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE
                                "," G_FILE_ATTRIBUTE_STANDARD_SIZE
                                "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                G_FILE_QUERY_INFO_NONE, nullptr, nullptr)
            : nullptr;
    g_object_unref(file);
    char *root = xd_thumb_cache_dir();
    int status = EXIT_CANT;
    /* Only regular files (a FIFO would block the reader), never the
     * cache's own. */
    if (info != nullptr && g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR &&
        !xd_thumb_in_cache(root, path))
        status = make(path, uri, info, root, size, verbose);
    else if (verbose)
        fprintf(stderr, "xd-thumbnailer: %s: not a local file to thumbnail\n", arg);
    g_clear_object(&info);
    g_free(root);
    g_free(uri);
    g_free(path);
    return status;
}

int main(int argc, char **argv) {
    char *size_name = nullptr;
    char *decode_size = nullptr;
    char **rest = nullptr;
    gboolean verbose = FALSE;
    GOptionEntry entries[] = {
        {"size", 's', 0, G_OPTION_ARG_STRING, &size_name,
         "normal (128), large (256, the default), x-large (512) or xx-large (1024)", "SIZE"},
        {"verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Say why a file wasn't thumbnailed",
         nullptr},
        {"decode", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING, &decode_size, nullptr, nullptr},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &rest, nullptr, "FILE"},
        G_OPTION_ENTRY_NULL,
    };
    GOptionContext *ctx = g_option_context_new("- make a file's thumbnail");
    g_option_context_add_main_entries(ctx, entries, nullptr);
    GError *err = nullptr;
    gboolean parsed = g_option_context_parse(ctx, &argc, &argv, &err);
    g_option_context_free(ctx);
    int status = EXIT_BROKEN;
    XdThumbSize size = XD_THUMB_LARGE;
    if (!parsed) {
        fprintf(stderr, "xd-thumbnailer: %s\n", err->message);
        g_error_free(err);
    } else if (decode_size != nullptr) {
        status = xd_thumb_size_parse(decode_size, &size) ? decode(size, rest) : EXIT_DECODE_FAILED;
    } else if (size_name != nullptr && !xd_thumb_size_parse(size_name, &size)) {
        fprintf(stderr, "xd-thumbnailer: unknown size %s\n", size_name);
    } else if (rest == nullptr || rest[0] == nullptr || rest[1] != nullptr) {
        fprintf(stderr, "Usage: xd-thumbnailer [--size=SIZE] FILE\n");
    } else {
        status = thumbnail(rest[0], size, verbose);
    }
    g_free(size_name);
    g_free(decode_size);
    g_strfreev(rest);
    return status;
}
