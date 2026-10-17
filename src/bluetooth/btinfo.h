/* SPDX-License-Identifier: MIT
 * What Bluetooth Devices shows about a device or radio, from the numbers
 * BlueZ gives: the Class of Device and GAP Appearance (what kind of
 * device, and its icon), service UUIDs (their names), and the radio's
 * HCI version (the Bluetooth version).
 */
#ifndef XD_BLUETOOTH_BTINFO_H
#define XD_BLUETOOTH_BTINFO_H

#include <stdint.h>

typedef struct xd_bt_type {
    const char *icon; /* icon theme name */
    const char *name; /* "Laptop computer", "Headset", ... */
} xd_bt_type;

/* From the Class of Device (Device1.Class); {nullptr, nullptr} when it
 * says nothing (0, or Miscellaneous/Uncategorized). */
xd_bt_type xd_bt_class_type(uint32_t cls);
/* From a Bluetooth LE device's GAP Appearance (Device1.Appearance). */
xd_bt_type xd_bt_appearance_type(uint16_t appearance);
/* From BlueZ's Icon property ("audio-headset", ...), for when neither
 * of the above helps; always gives something. */
xd_bt_type xd_bt_icon_type(const char *icon);

/* The service a UUID stands for ("Advanced Audio", "Human Interface
 * Device", ...): the Bluetooth SIG's 16-bit UUIDs in their 128-bit form,
 * either case. nullptr for others. */
const char *xd_bt_uuid_name(const char *uuid);

/* The Bluetooth Core version of an HCI/LMP version number ("2.1 + EDR",
 * "5.3", ...); nullptr when unknown. */
const char *xd_bt_version_name(int hci_version);

#endif
