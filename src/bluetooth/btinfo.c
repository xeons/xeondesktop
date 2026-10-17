/* SPDX-License-Identifier: MIT
 * Numbers from the Bluetooth SIG's Assigned Numbers: Class of Device
 * (section 2.8), Appearance values (2.6), 16-bit service UUIDs (3.3,
 * 3.4) and HCI versions (2.1).
 */
#include <glib.h>
#include <stdlib.h>
#include <string.h>

#include "../core/common.h"
#include "btinfo.h"

static constexpr xd_bt_type NONE = {nullptr, nullptr};
static const xd_bt_type DEFAULT = {"bluetooth", "Bluetooth device"};

static xd_bt_type type(const char *icon, const char *name) { return (xd_bt_type){icon, name}; }

static xd_bt_type computer_class(unsigned minor) {
    switch (minor) {
    case 1:
        return type("computer", "Desktop computer");
    case 2:
        return type("computer", "Server");
    case 3:
        return type("computer-laptop", "Laptop computer");
    case 4:
    case 5:
        return type("pda", "Handheld computer");
    case 6:
        return type("computer", "Wearable computer");
    case 7:
        return type("computer", "Tablet");
    default:
        return type("computer", "Computer");
    }
}

static xd_bt_type phone_class(unsigned minor) {
    switch (minor) {
    case 1:
        return type("phone", "Cellular phone");
    case 2:
        return type("phone", "Cordless phone");
    case 3:
        return type("phone", "Smartphone");
    case 4:
        return type("modem", "Modem or voice gateway");
    case 5:
        return type("modem", "ISDN access point");
    default:
        return type("phone", "Phone");
    }
}

static xd_bt_type audio_class(unsigned minor) {
    switch (minor) {
    case 1:
        return type("audio-headset", "Headset");
    case 2:
        return type("audio-headset", "Hands-free device");
    case 4:
        return type("audio-input-microphone", "Microphone");
    case 5:
        return type("audio-speakers", "Loudspeaker");
    case 6:
        return type("audio-headphones", "Headphones");
    case 7:
        return type("multimedia-player", "Portable audio player");
    case 8:
        return type("audio-card", "Car audio");
    case 9:
        return type("video-display", "Set-top box");
    case 10:
        return type("audio-speakers", "Hi-fi audio");
    case 11:
        return type("camera-video", "VCR");
    case 12:
        return type("camera-video", "Video camera");
    case 13:
        return type("camera-video", "Camcorder");
    case 14:
        return type("video-display", "Video monitor");
    case 15:
        return type("video-display", "Video display and loudspeaker");
    case 16:
        return type("camera-web", "Video conferencing");
    case 18:
        return type("input-gaming", "Gaming toy");
    default:
        return type("audio-card", "Audio/video device");
    }
}

/* Peripherals: bits 6-7 of the minor class say keyboard and/or pointing
 * device, bits 2-5 the kind. */
static xd_bt_type peripheral_class(unsigned minor) {
    switch (minor & 0x0f) {
    case 1:
        return type("input-gaming", "Joystick");
    case 2:
        return type("input-gaming", "Game pad");
    case 3:
        return type("input-remote", "Remote control");
    case 4:
        return type("input-peripheral", "Sensing device");
    case 5:
        return type("input-tablet", "Digitizer tablet");
    case 6:
        return type("media-flash", "Card reader");
    case 7:
        return type("input-tablet", "Digital pen");
    default:
        break;
    }
    switch (minor >> 4) {
    case 1:
        return type("input-keyboard", "Keyboard");
    case 2:
        return type("input-mouse", "Mouse");
    case 3:
        return type("input-keyboard", "Keyboard and mouse");
    default:
        return type("input-peripheral", "Input device");
    }
}

/* Imaging: bits 4-7 of the class, the first one set wins (printers
 * first, as they're what most such devices are). */
static xd_bt_type imaging_class(unsigned minor) {
    if (minor & 0x20)
        return type("printer", "Printer");
    if (minor & 0x10)
        return type("scanner", "Scanner");
    if (minor & 0x08)
        return type("camera-photo", "Camera");
    if (minor & 0x04)
        return type("video-display", "Display");
    return type("printer", "Imaging device");
}

xd_bt_type xd_bt_class_type(uint32_t cls) {
    unsigned major = (cls >> 8) & 0x1f;
    unsigned minor = (cls >> 2) & 0x3f;
    switch (major) {
    case 1:
        return computer_class(minor);
    case 2:
        return phone_class(minor);
    case 3:
        return type("network-wireless", "Network access point");
    case 4:
        return audio_class(minor);
    case 5:
        return peripheral_class(minor);
    case 6:
        return imaging_class(minor);
    case 7:
        return type("wearable", "Wearable device");
    case 8:
        return type("input-gaming", "Toy");
    case 9:
        return type("health", "Health device");
    default:
        return NONE; /* Miscellaneous, Uncategorized */
    }
}

xd_bt_type xd_bt_appearance_type(uint16_t appearance) {
    unsigned sub = appearance & 0x3f;
    switch (appearance >> 6) {
    case 0x01:
        return type("phone", "Phone");
    case 0x02:
        return type("computer", "Computer");
    case 0x03:
        return type("wearable", "Watch");
    case 0x05:
        return type("video-display", "Display");
    case 0x06:
        return type("input-remote", "Remote control");
    case 0x0a:
        return type("multimedia-player", "Media player");
    case 0x0b:
        return type("scanner", "Barcode scanner");
    case 0x0d:
        return type("health", "Heart rate sensor");
    case 0x0f: /* HID */
        switch (sub) {
        case 1:
            return type("input-keyboard", "Keyboard");
        case 2:
            return type("input-mouse", "Mouse");
        case 3:
            return type("input-gaming", "Joystick");
        case 4:
            return type("input-gaming", "Game pad");
        case 5:
            return type("input-tablet", "Digitizer tablet");
        case 6:
            return type("media-flash", "Card reader");
        case 7:
            return type("input-tablet", "Digital pen");
        case 8:
            return type("scanner", "Barcode scanner");
        default:
            return type("input-peripheral", "Input device");
        }
    case 0x21:
        return type("audio-speakers", "Speaker");
    case 0x22:
        return type("audio-input-microphone", "Microphone");
    case 0x25: /* wearable audio */
        switch (sub) {
        case 1:
            return type("audio-headphones", "Earbuds");
        case 2:
            return type("audio-headset", "Headset");
        case 3:
            return type("audio-headphones", "Headphones");
        default:
            return type("audio-headset", "Wearable audio device");
        }
    case 0x29:
        return type("audio-headphones", "Hearing aid");
    default:
        return NONE;
    }
}

xd_bt_type xd_bt_icon_type(const char *icon) {
    /* The names BlueZ gives (src/dbus-common.c's class_to_icon and
     * gap_appearance_to_icon). */
    static const xd_bt_type icons[] = {
        {"computer", "Computer"},
        {"phone", "Phone"},
        {"modem", "Modem"},
        {"network-wireless", "Network access point"},
        {"audio-headset", "Headset"},
        {"audio-headphones", "Headphones"},
        {"audio-card", "Audio device"},
        {"camera-video", "Video camera"},
        {"camera-photo", "Camera"},
        {"video-display", "Display"},
        {"multimedia-player", "Media player"},
        {"input-keyboard", "Keyboard"},
        {"input-mouse", "Mouse"},
        {"input-gaming", "Game controller"},
        {"input-tablet", "Tablet"},
        {"printer", "Printer"},
        {"scanner", "Scanner"},
    };
    for (size_t i = 0; icon != nullptr && i < XD_ARRAY_LEN(icons); i++)
        if (strcmp(icon, icons[i].icon) == 0)
            return icons[i];
    return DEFAULT;
}

typedef struct service {
    uint16_t id;
    const char *name;
} service;

/* Sorted by id. */
static const service services[] = {
    {0x1101, "Serial Port"},
    {0x1102, "LAN Access Using PPP"},
    {0x1103, "Dial-up Networking"},
    {0x1104, "IrMC Synchronization"},
    {0x1105, "Object Push"},
    {0x1106, "File Transfer"},
    {0x1108, "Headset"},
    {0x110a, "Audio Source"},
    {0x110b, "Audio Sink"},
    {0x110c, "Remote Control Target"},
    {0x110d, "Advanced Audio"},
    {0x110e, "Remote Control"},
    {0x110f, "Remote Control Controller"},
    {0x1112, "Headset Audio Gateway"},
    {0x1115, "Personal Area Network User"},
    {0x1116, "Network Access Point"},
    {0x1117, "Group Ad-hoc Network"},
    {0x111e, "Hands-free"},
    {0x111f, "Hands-free Audio Gateway"},
    {0x1124, "Human Interface Device"},
    {0x1126, "Printing"},
    {0x112d, "SIM Access"},
    {0x112f, "Phonebook Access"},
    {0x1131, "Headset"},
    {0x1132, "Message Access"},
    {0x1133, "Message Notification"},
    {0x1200, "PnP Information"},
    {0x1203, "Generic Audio"},
    {0x1800, "Generic Access"},
    {0x1801, "Generic Attribute"},
    {0x180a, "Device Information"},
    {0x180f, "Battery"},
    {0x1812, "Human Interface Device"},
    {0x1844, "Volume Control"},
    {0x1846, "Coordinated Set Identification"},
    {0x184e, "Audio Stream Control"},
    {0x184f, "Broadcast Audio Scan"},
    {0x1850, "Published Audio Capabilities"},
};

static int by_id(const void *key, const void *elem) {
    unsigned a = *(const uint16_t *)key, b = ((const service *)elem)->id;
    return a < b ? -1 : a > b ? 1 : 0;
}

const char *xd_bt_uuid_name(const char *uuid) {
    /* 0000xxxx-0000-1000-8000-00805f9b34fb: the Bluetooth Base UUID. */
    static const char base[] = "-0000-1000-8000-00805f9b34fb";
    if (uuid == nullptr || !g_uuid_string_is_valid(uuid) || strncmp(uuid, "0000", 4) != 0 ||
        g_ascii_strcasecmp(uuid + 8, base) != 0)
        return nullptr;
    uint16_t id = 0;
    for (size_t i = 4; i < 8; i++)
        id = (uint16_t)((unsigned)id << 4 | (unsigned)g_ascii_xdigit_value(uuid[i]));
    const service *s = bsearch(&id, services, XD_ARRAY_LEN(services), sizeof(services[0]), by_id);
    return s != nullptr ? s->name : nullptr;
}

const char *xd_bt_version_name(int hci_version) {
    static const char *const versions[] = {
        "1.0b", "1.1", "1.2", "2.0 + EDR", "2.1 + EDR", "3.0 + HS", "4.0", "4.1",
        "4.2",  "5.0", "5.1", "5.2",       "5.3",       "5.4",      "6.0", "6.1",
    };
    if (hci_version < 0 || (size_t)hci_version >= XD_ARRAY_LEN(versions))
        return nullptr;
    return versions[hci_version];
}
