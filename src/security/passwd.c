/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "passwd.h"

struct xd_passwd {
    pid_t pid;
    int fd;
    char *old_password;
    char *new_password;
    GString *line;    /* output since the last newline */
    char *message;    /* last line passwd printed */
    char *bad;        /* last "BAD PASSWORD" line, if any */
    bool auth_error;  /* pam_unix's answer to a wrong current password */
    int old_prompts;  /* answers given */
    int new_prompts;
    xd_passwd_result result;
};

xd_passwd *xd_passwd_start(const char *program, const char *old_password,
                           const char *new_password, GError **error) {
    if (program == nullptr)
        program = "passwd";
    /* A line-mode terminal that never echoes, so an answer typed before
     * passwd turns echo off can't come back to us (or into a message). */
    struct termios tio = {0};
    tio.c_iflag = ICRNL;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CS8 | CREAD;
    tio.c_lflag = ICANON | ISIG;
    tio.c_cc[VINTR] = 3;
    tio.c_cc[VEOF] = 4;
    tio.c_cc[VERASE] = 0x7f;
    tio.c_cc[VMIN] = 1;
    cfsetspeed(&tio, B38400);
    int fd = -1;
    pid_t pid = forkpty(&fd, nullptr, &tio, nullptr);
    if (pid < 0) {
        int e = errno;
        GFileError code = g_file_error_from_errno(e);
        g_set_error(error, G_FILE_ERROR, (gint)code, "cannot start %s: %s",
                    program, g_strerror(e));
        return nullptr;
    }
    if (pid == 0) {
        /* Prompts we can recognise. */
        setenv("LC_ALL", "C", 1);
        setenv("LANGUAGE", "C", 1);
        execlp(program, program, (char *)nullptr);
        _exit(127);
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    xd_passwd *p = g_new0(xd_passwd, 1);
    p->pid = pid;
    p->fd = fd;
    p->old_password = g_strdup(old_password);
    p->new_password = g_strdup(new_password);
    p->line = g_string_new(nullptr);
    p->message = g_strdup("");
    p->result = XD_PASSWD_RUNNING;
    return p;
}

int xd_passwd_fd(const xd_passwd *p) { return p->fd; }

const char *xd_passwd_message(const xd_passwd *p) { return p->message; }

static void answer(xd_passwd *p, const char *text) {
    GString *s = g_string_new(text);
    g_string_append_c(s, '\n');
    for (size_t done = 0; done < s->len;) {
        ssize_t n = write(p->fd, s->str + done, s->len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    explicit_bzero(s->str, s->len);
    g_string_free(s, TRUE);
}

static void stop(xd_passwd *p, xd_passwd_result result) {
    if (p->pid > 0) {
        kill(p->pid, SIGTERM);
        waitpid(p->pid, nullptr, 0);
        p->pid = 0;
    }
    p->result = result;
}

/* A finished line of output. */
static void note_line(xd_passwd *p, const char *text) {
    char *t = g_strstrip(g_strdup(text));
    if (t[0] != '\0' && !g_str_has_prefix(t, "Changing password for")) {
        g_free(p->message);
        p->message = g_strdup(t);
        if (g_str_has_prefix(t, "BAD PASSWORD")) {
            g_free(p->bad);
            p->bad = g_strdup(t);
        }
        char *lower = g_ascii_strdown(t, -1);
        if (strstr(lower, "authentication token manipulation") != nullptr ||
            strstr(lower, "authentication failure") != nullptr)
            p->auth_error = true;
        g_free(lower);
    }
    g_free(t);
}

/* The unfinished line: answer it if it's a password prompt. */
static void check_prompt(xd_passwd *p) {
    char *t = g_ascii_strdown(p->line->str, (gssize)p->line->len);
    g_strstrip(t);
    bool prompt = g_str_has_suffix(t, ":") && strstr(t, "password") != nullptr;
    if (prompt) {
        if (strstr(t, "retype") != nullptr || strstr(t, "again") != nullptr ||
            strstr(t, "repeat") != nullptr) {
            answer(p, p->new_password);
        } else if (strstr(t, "new") != nullptr) {
            if (++p->new_prompts > 1)
                stop(p, XD_PASSWD_REJECTED);
            else
                answer(p, p->new_password);
        } else {
            if (++p->old_prompts > 1)
                stop(p, XD_PASSWD_WRONG_OLD);
            else
                answer(p, p->old_password);
        }
        g_string_truncate(p->line, 0);
    }
    g_free(t);
}

static xd_passwd_result finish(xd_passwd *p) {
    if (p->line->len > 0)
        note_line(p, p->line->str);
    int status = 0;
    pid_t r;
    do {
        r = waitpid(p->pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    p->pid = 0;
    if (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return XD_PASSWD_OK;
    if (p->bad != nullptr) {
        g_free(p->message);
        p->message = g_strdup(p->bad);
        return XD_PASSWD_REJECTED;
    }
    if (p->auth_error && p->new_prompts == 0)
        return XD_PASSWD_WRONG_OLD;
    return XD_PASSWD_FAILED;
}

xd_passwd_result xd_passwd_step(xd_passwd *p) {
    if (p->result != XD_PASSWD_RUNNING)
        return p->result;
    char buf[512];
    ssize_t n = read(p->fd, buf, sizeof(buf));
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return XD_PASSWD_RUNNING;
    if (n <= 0) {
        /* EIO: passwd closed the terminal. */
        p->result = finish(p);
        return p->result;
    }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            note_line(p, p->line->str);
            g_string_truncate(p->line, 0);
        } else if (buf[i] != '\r') {
            g_string_append_c(p->line, buf[i]);
        }
    }
    check_prompt(p);
    return p->result;
}

void xd_passwd_free(xd_passwd *p) {
    if (p == nullptr)
        return;
    if (p->pid > 0)
        stop(p, XD_PASSWD_FAILED);
    close(p->fd);
    explicit_bzero(p->old_password, strlen(p->old_password));
    explicit_bzero(p->new_password, strlen(p->new_password));
    g_free(p->old_password);
    g_free(p->new_password);
    g_string_free(p->line, TRUE);
    g_free(p->message);
    g_free(p->bad);
    g_free(p);
}
