/* SPDX-License-Identifier: MIT
 * The parts of User Accounts (xd-users) that don't need a window: user
 * names, password hashes for AccountsService, and account pictures.
 */
#ifndef XD_USERS_USERS_H
#define XD_USERS_USERS_H

#include <gdk-pixbuf/gdk-pixbuf.h>

/* AccountsService's account types. */
enum { XD_ACCOUNT_STANDARD = 0, XD_ACCOUNT_ADMIN = 1 };

/* Whether `name` is a user name every distribution's useradd takes (and
 * Debian's adduser): a lowercase letter, then lowercase letters, digits,
 * "_" and "-", at most 32. */
bool xd_users_valid_name(const char *name);
/* A user name for `full_name` ("Jürgen Smith" -> "jurgensmith"), or ""
 * if nothing usable is left. */
char *xd_users_suggest_name(const char *full_name);

/* `password` hashed with libxcrypt's default (preferred) method, as
 * AccountsService's SetPassword wants it; nullptr on failure. */
char *xd_users_hash_password(const char *password);

/* The account picture from `source` (any image gdk-pixbuf reads): the
 * centre square scaled to `size` px, saved as PNG at `dest`.
 * AccountsService takes pictures up to 1 MB, and the Start menu shows
 * ~/.face. */
bool xd_users_make_picture(const char *source, const char *dest, int size, GError **error);

#endif
