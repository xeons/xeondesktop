/* SPDX-License-Identifier: MIT
 * The password box cupsd's own authorization asks for (Pause Printing,
 * Delete, Add Printer, ...): XP's "Connect to" box, a user name and a
 * password, handed to libcups through cupsSetPasswordCB2.
 */
#ifndef XD_PRINTERS_AUTH_H
#define XD_PRINTERS_AUTH_H

/* Installs the box for the main thread's requests. */
void xd_cups_auth_init(void);
/* Call before each change: a new operation, so the box asks afresh
 * (and gives up after three wrong passwords). */
void xd_cups_auth_reset(void);

#endif
