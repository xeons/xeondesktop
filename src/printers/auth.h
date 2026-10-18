/* SPDX-License-Identifier: MIT
 * The password box cupsd's own authorization asks for (Pause Printing,
 * Delete, Add Printer, ...): XP's "Connect to" box, a user name and a
 * password, handed to libcups through cupsSetPasswordCB2.
 */
#ifndef XD_PRINTERS_AUTH_H
#define XD_PRINTERS_AUTH_H

/* The main thread's requests (reads only) ask nobody: libcups would ask
 * on the terminal. */
void xd_cups_auth_init(void);

/* Changes are made in worker threads (xd_printers_run), so the windows
 * don't freeze while the scheduler takes its time (libcups's callback is
 * per thread): between these, the worker's requests ask with the box,
 * shown on the main thread while the worker waits for the answer. Begin
 * is a new operation, so the box asks afresh (and gives up after three
 * wrong passwords); end leaves it asking nobody, as listing threads do. */
void xd_cups_auth_worker_begin(void);
void xd_cups_auth_worker_end(void);

#endif
