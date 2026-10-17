"""systemd-localed (org.freedesktop.locale1) for python-dbusmock, which has
no template of its own: the properties and setters xd-intl uses.

    python3 -m dbusmock --system -t tests/dbusmock/localed.py
"""

# SPDX-License-Identifier: MIT

import dbus

BUS_NAME = "org.freedesktop.locale1"
MAIN_OBJ = "/org/freedesktop/locale1"
MAIN_IFACE = "org.freedesktop.locale1"
SYSTEM_BUS = True


def load(mock, parameters):
    mock.AddMethods(
        MAIN_IFACE,
        [
            ("SetLocale", "asb", "", f'self.Set("{MAIN_IFACE}", "Locale", args[0])'),
            (
                "SetVConsoleKeyboard",
                "ssbb",
                "",
                f'self.Set("{MAIN_IFACE}", "VConsoleKeymap", args[0]); '
                f'self.Set("{MAIN_IFACE}", "VConsoleKeymapToggle", args[1])',
            ),
            (
                "SetX11Keyboard",
                "ssssbb",
                "",
                f'self.Set("{MAIN_IFACE}", "X11Layout", args[0]); '
                f'self.Set("{MAIN_IFACE}", "X11Model", args[1]); '
                f'self.Set("{MAIN_IFACE}", "X11Variant", args[2]); '
                f'self.Set("{MAIN_IFACE}", "X11Options", args[3]); '
                # localed converts to the closest console keymap; the layout
                # name will do here.
                f'args[4] and self.Set("{MAIN_IFACE}", "VConsoleKeymap", args[0].split(",")[0])',
            ),
        ],
    )
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "Locale": dbus.Array(parameters.get("Locale", ["LANG=en_US.UTF-8"]), signature="s"),
                "VConsoleKeymap": parameters.get("VConsoleKeymap", "us"),
                "VConsoleKeymapToggle": parameters.get("VConsoleKeymapToggle", ""),
                "X11Layout": parameters.get("X11Layout", "us"),
                "X11Model": parameters.get("X11Model", "pc105"),
                "X11Variant": parameters.get("X11Variant", ""),
                "X11Options": parameters.get("X11Options", ""),
            },
            signature="sv",
        ),
    )
