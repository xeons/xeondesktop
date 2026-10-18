"""switcheroo-control (net.hadess.SwitcherooControl) for python-dbusmock,
which has no template of its own: a hybrid graphics laptop, an integrated
card (the default) and a dedicated one whose programs get DRI_PRIME=1.

    python3 -m dbusmock --system -t tests/dbusmock/switcheroo.py

Parameters: "GPUs", a list of {"Name", "Environment", "Default"} dicts;
"HasDualGpu" (default: more than one GPU).
"""

# SPDX-License-Identifier: MIT

import dbus

BUS_NAME = "net.hadess.SwitcherooControl"
MAIN_OBJ = "/net/hadess/SwitcherooControl"
MAIN_IFACE = "net.hadess.SwitcherooControl"
SYSTEM_BUS = True

DEFAULT_GPUS = [
    {"Name": "Intel Corporation UHD Graphics 620", "Environment": ["DRI_PRIME", "0"],
     "Default": True},
    {"Name": "NVIDIA Corporation GP108M [GeForce MX150]",
     "Environment": ["DRI_PRIME", "1", "__GLX_VENDOR_LIBRARY_NAME", "mesa"], "Default": False},
]  # fmt: skip


def gpu(d):
    return dbus.Dictionary(
        {
            "Name": dbus.String(d["Name"]),
            "Environment": dbus.Array(d["Environment"], signature="s"),
            "Default": dbus.Boolean(d["Default"]),
        },
        signature="sv",
    )


def load(mock, parameters):
    gpus = parameters.get("GPUs", DEFAULT_GPUS)
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {
                "HasDualGpu": dbus.Boolean(parameters.get("HasDualGpu", len(gpus) > 1)),
                "NumGPUs": dbus.UInt32(len(gpus)),
                "GPUs": dbus.Array([gpu(g) for g in gpus], signature="a{sv}"),
            },
            signature="sv",
        ),
    )
