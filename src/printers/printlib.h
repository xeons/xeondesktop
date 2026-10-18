/* SPDX-License-Identifier: MIT
 * Printers and Faxes (xd-printers): what CUPS reports, in XP's words.
 * No GTK here, so the unit tests can check it.
 */
#ifndef XD_PRINTERS_PRINTLIB_H
#define XD_PRINTERS_PRINTLIB_H

#include <glib.h>

/* The Status column: "Ready", "Printing", "Paused", "Offline", "Paper
 * Out", ... from IPP's printer-state (3 idle, 4 processing, 5 stopped)
 * and printer-state-reasons (comma-separated, as cupsGetDests gives
 * them; nullptr or "none" for none). */
const char *xd_printer_status_text(int state, const char *reasons, bool accepting);

/* Whether the printer is paused (stopped, with the "paused" reason). */
bool xd_printer_is_paused(int state, const char *reasons);

/* The print queue's Status column, from job-state (3 pending ... 9
 * completed): "" for a job waiting its turn, "Paused", "Printing", ... */
const char *xd_print_job_status_text(int state);

/* "123 bytes", "42.5 KB", "1.37 MB": three significant digits, rounded
 * down, as Explorer shows sizes. Free with g_free. */
char *xd_print_size_text(guint64 bytes);

/* The Pages column: "3", "2/5" while printing, "N/A" when CUPS doesn't
 * know. Free with g_free. */
char *xd_print_pages_text(int done, int total, bool printing);

/* The Port column, from the device URI: "IP_192.168.1.5" for network
 * printers, the service name for DNS-SD ones, "USB" for USB, a file's
 * path. Free with g_free. */
char *xd_printer_port_text(const char *device_uri);

/* The Ports tab's Description: "Standard TCP/IP Port", "USB Port",
 * "Internet Printing (IPP)", ... */
const char *xd_printer_port_description(const char *device_uri);

/* Whether the device URI is a printer on the network (its icon has a
 * cable). */
bool xd_printer_uri_is_network(const char *device_uri);

/* Whether a typed device URI says where to print: a scheme and, for the
 * network's, a host ("socket://10.0.0.5", "usb://HP/LaserJet",
 * "file:/dev/null"; not "socket" or "ipp://"). */
bool xd_printer_device_uri_complete(const char *device_uri);
/* What Add Port puts in its Device URI box for a port CUPS-Get-Devices
 * found: the URI, or for a kind of port with no address yet (the bare
 * scheme, "socket") its start, "socket://". Free with g_free. */
char *xd_printer_device_uri_start(const char *device_uri);

/* Whether CUPS accepts `name` for a printer: 1 to 127 bytes, printable,
 * without spaces, '/', '\\', '#', '?', quotes. */
bool xd_printer_name_valid(const char *name);
/* A valid name made from a model or service name ("HP LaserJet 4" gives
 * "HP_LaserJet_4"). Free with g_free. */
char *xd_printer_name_suggest(const char *text);

/* Printing Preferences' choices in XP's words; nullptr when unknown. */
const char *xd_print_orientation_label(int orientation_requested);
const char *xd_print_sides_label(const char *sides);
const char *xd_print_color_label(const char *print_color_mode);
const char *xd_print_quality_label(int print_quality);

/* An IPP keyword nobody translated, made readable: "stationery-heavyweight"
 * gives "Stationery heavyweight". Free with g_free. */
char *xd_print_keyword_label(const char *keyword);

#endif
