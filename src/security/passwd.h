/* SPDX-License-Identifier: MIT
 * Changes the user's own password by running passwd(1) on a pseudo-
 * terminal and answering its prompts, as GNOME's Settings does: only the
 * setuid passwd may rewrite /etc/shadow, and it applies the system's PAM
 * password rules (pwquality and the like).
 *
 * passwd runs in the C locale so its prompts can be recognised: "Current
 * password:" gets the old password, "New password:" and "Retype new
 * password:" the new one. A second "Current" or "New" prompt means the
 * answer was refused (wrong old password, or a new one the rules reject),
 * so passwd is stopped there.
 */
#ifndef XD_SECURITY_PASSWD_H
#define XD_SECURITY_PASSWD_H

#include <glib.h>

typedef enum xd_passwd_result {
    XD_PASSWD_RUNNING,
    XD_PASSWD_OK,
    XD_PASSWD_WRONG_OLD, /* the old password was refused */
    XD_PASSWD_REJECTED,  /* the new password breaks the rules (see message) */
    XD_PASSWD_FAILED,    /* anything else (see message) */
} xd_passwd_result;

typedef struct xd_passwd xd_passwd;

/* Starts `program` (nullptr: "passwd") with no arguments. Returns nullptr
 * and sets *error if it can't. The passwords are copied and wiped later. */
xd_passwd *xd_passwd_start(const char *program, const char *old_password,
                           const char *new_password, GError **error);
/* The pty to poll for reading. */
int xd_passwd_fd(const xd_passwd *passwd);
/* Call when the fd is readable (or hung up): reads, answers prompts. */
xd_passwd_result xd_passwd_step(xd_passwd *passwd);
/* What passwd last said, e.g. "BAD PASSWORD: The password is shorter than
 * 8 characters". Empty if nothing. */
const char *xd_passwd_message(const xd_passwd *passwd);
/* Stops passwd if it's still running and wipes the passwords. */
void xd_passwd_free(xd_passwd *passwd);

#endif
