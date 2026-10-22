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
        int error_number = errno;
        GFileError code = g_file_error_from_errno(error_number);
        g_set_error(error, G_FILE_ERROR, (gint)code, "cannot start %s: %s", program,
                    g_strerror(error_number));
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
    xd_passwd *passwd = g_new0(xd_passwd, 1);
    passwd->pid = pid;
    passwd->fd = fd;
    passwd->old_password = g_strdup(old_password);
    passwd->new_password = g_strdup(new_password);
    passwd->line = g_string_new(nullptr);
    passwd->message = g_strdup("");
    passwd->result = XD_PASSWD_RUNNING;
    return passwd;
}

int xd_passwd_fd(const xd_passwd *passwd) { return passwd->fd; }

const char *xd_passwd_message(const xd_passwd *passwd) { return passwd->message; }

static void answer(xd_passwd *passwd, const char *text) {
    GString *line = g_string_new(text);
    g_string_append_c(line, '\n');
    for (size_t done = 0; done < line->len;) {
        ssize_t written = write(passwd->fd, line->str + done, line->len - done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        done += (size_t)written;
    }
    explicit_bzero(line->str, line->len);
    g_string_free(line, TRUE);
}

static void stop(xd_passwd *passwd, xd_passwd_result result) {
    if (passwd->pid > 0) {
        kill(passwd->pid, SIGTERM);
        waitpid(passwd->pid, nullptr, 0);
        passwd->pid = 0;
    }
    passwd->result = result;
}

/* A finished line of output. */
static void note_line(xd_passwd *passwd, const char *text) {
    char *line = g_strstrip(g_strdup(text));
    if (line[0] != '\0' && !g_str_has_prefix(line, "Changing password for")) {
        g_free(passwd->message);
        passwd->message = g_strdup(line);
        if (g_str_has_prefix(line, "BAD PASSWORD")) {
            g_free(passwd->bad);
            passwd->bad = g_strdup(line);
        }
        char *lower = g_ascii_strdown(line, -1);
        if (strstr(lower, "authentication token manipulation") != nullptr ||
            strstr(lower, "authentication failure") != nullptr)
            passwd->auth_error = true;
        g_free(lower);
    }
    g_free(line);
}

/* The unfinished line: answer it if it's a password prompt. */
static void check_prompt(xd_passwd *passwd) {
    char *line = g_ascii_strdown(passwd->line->str, (gssize)passwd->line->len);
    g_strstrip(line);
    bool prompt = g_str_has_suffix(line, ":") && strstr(line, "password") != nullptr;
    if (prompt) {
        if (strstr(line, "retype") != nullptr || strstr(line, "again") != nullptr ||
            strstr(line, "repeat") != nullptr) {
            answer(passwd, passwd->new_password);
        } else if (strstr(line, "new") != nullptr) {
            if (++passwd->new_prompts > 1)
                stop(passwd, XD_PASSWD_REJECTED);
            else
                answer(passwd, passwd->new_password);
        } else {
            if (++passwd->old_prompts > 1)
                stop(passwd, XD_PASSWD_WRONG_OLD);
            else
                answer(passwd, passwd->old_password);
        }
        g_string_truncate(passwd->line, 0);
    }
    g_free(line);
}

static xd_passwd_result finish(xd_passwd *passwd) {
    if (passwd->line->len > 0)
        note_line(passwd, passwd->line->str);
    int status = 0;
    pid_t pid;
    do {
        pid = waitpid(passwd->pid, &status, 0);
    } while (pid < 0 && errno == EINTR);
    passwd->pid = 0;
    if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return XD_PASSWD_OK;
    if (passwd->bad != nullptr) {
        g_free(passwd->message);
        passwd->message = g_strdup(passwd->bad);
        return XD_PASSWD_REJECTED;
    }
    if (passwd->auth_error && passwd->new_prompts == 0)
        return XD_PASSWD_WRONG_OLD;
    return XD_PASSWD_FAILED;
}

xd_passwd_result xd_passwd_step(xd_passwd *passwd) {
    if (passwd->result != XD_PASSWD_RUNNING)
        return passwd->result;
    char buf[512];
    ssize_t bytes_read = read(passwd->fd, buf, sizeof(buf));
    if (bytes_read < 0 && (errno == EINTR || errno == EAGAIN))
        return XD_PASSWD_RUNNING;
    if (bytes_read <= 0) {
        /* EIO: passwd closed the terminal. */
        passwd->result = finish(passwd);
        return passwd->result;
    }
    for (ssize_t i = 0; i < bytes_read; i++) {
        if (buf[i] == '\n') {
            note_line(passwd, passwd->line->str);
            g_string_truncate(passwd->line, 0);
        } else if (buf[i] != '\r') {
            g_string_append_c(passwd->line, buf[i]);
        }
    }
    check_prompt(passwd);
    return passwd->result;
}

void xd_passwd_free(xd_passwd *passwd) {
    if (passwd == nullptr)
        return;
    if (passwd->pid > 0)
        stop(passwd, XD_PASSWD_FAILED);
    close(passwd->fd);
    explicit_bzero(passwd->old_password, strlen(passwd->old_password));
    explicit_bzero(passwd->new_password, strlen(passwd->new_password));
    g_free(passwd->old_password);
    g_free(passwd->new_password);
    g_string_free(passwd->line, TRUE);
    g_free(passwd->message);
    g_free(passwd->bad);
    g_free(passwd);
}
