/* SPDX-License-Identifier: MIT
 * xd-bluetooth's parts: the property sheet (main.c), a device's
 * Properties (devprops.c), the Add Bluetooth Device Wizard (wizard.c) and
 * the pairing agent BlueZ asks for passkeys (agent.c).
 */
#ifndef XD_BLUETOOTH_BT_H
#define XD_BLUETOOTH_BT_H

#include <gtk/gtk.h>

#include "btinfo.h"

#define BLUEZ "org.bluez"
#define IFACE_ADAPTER "org.bluez.Adapter1"
#define IFACE_DEVICE "org.bluez.Device1"

#define BT_TITLE "Bluetooth Devices"

typedef struct bt_wizard bt_wizard;
typedef struct bt_request bt_request;

typedef struct bt_app {
    GtkWidget *window, *apply, *tabs;
    /* BlueZ's objects (adapters, devices); nullptr without a system bus. */
    GDBusObjectManager *objects;
    GDBusConnection *bus;
    bool quit_with_wizard; /* --add: only the wizard */

    /* Devices */
    GtkListStore *devices;
    GtkWidget *device_view, *add, *remove, *properties;
    /* Options */
    GtkWidget *discoverable, *pairable, *show_icon, *alias, *options_note;
    bool updating;
    /* Hardware */
    GtkListStore *radios;
    GtkWidget *radio_view, *maker, *model, *address, *version, *location, *status;

    /* The agent: registered while BlueZ runs. */
    unsigned agent_id;
    bool agent_registered;
    bt_request *request; /* the question BlueZ is waiting on */
    GtkWidget *display;  /* a passkey shown for typing on the device */
    /* While the wizard pairs: the device, and the passkey it chose for
     * devices that ask for one (Bluetooth 2.0 and older). */
    char *pairing_path, *pin;
    bt_wizard *wizard;
} bt_app;

/* ---- main.c: BlueZ helpers --------------------------------------------- */

/* The first adapter (most machines have one); caller unrefs. */
GDBusInterface *bt_adapter(bt_app *state);
/* The interface of the object at `path`; caller unrefs. */
GDBusInterface *bt_interface(bt_app *state, const char *path, const char *iface);
/* Cached properties; missing or mistyped ones read as false / nullptr / 0. */
bool bt_prop_b(GDBusInterface *dbus_interface, const char *name);
char *bt_prop_s(GDBusInterface *dbus_interface, const char *name); /* strings and paths; g_free */
uint32_t bt_prop_u(GDBusInterface *dbus_interface, const char *name);
/* Alias, else Address; g_free. */
char *bt_device_name(GDBusInterface *dev);
/* Its kind and icon from Class, Appearance or Icon. */
xd_bt_type bt_device_type(GDBusInterface *dev);
/* Sets a property; errors are shown over `parent`. */
void bt_set_prop(bt_app *state, GtkWindow *parent, const char *path, const char *iface,
                 const char *name, GVariant *value);
void bt_error(GtkWindow *parent, const char *what, const GError *error);

/* ---- devprops.c -------------------------------------------------------- */

void bt_device_properties(bt_app *state, const char *path);

/* ---- wizard.c ---------------------------------------------------------- */

/* Opens the Add Bluetooth Device Wizard (or raises the open one). */
void bt_wizard_open(bt_app *state);
/* The wizard's window, nullptr when it's closed. */
GtkWindow *bt_wizard_window(bt_app *state);

/* ---- agent.c ----------------------------------------------------------- */

/* Registers org.bluez.Agent1 when BlueZ is there (and again whenever it
 * restarts); unregisters at exit. */
void bt_agent_start(bt_app *state);
void bt_agent_stop(bt_app *state);
/* Closes a passkey shown for the device, once pairing is over. */
void bt_agent_pairing_done(bt_app *state);

#endif
