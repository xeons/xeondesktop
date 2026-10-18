"""systemd-timesyncd's manager (org.freedesktop.timesync1) for
python-dbusmock, with what Date and Time Properties uses: the server
lists, the server in use, the last NTP answer and SetRuntimeNTPServers.

    python3 -m dbusmock --system -t tests/dbusmock/timesync.py

Parameters: "ServerName", "SystemNTPServers", "FallbackNTPServers",
"LinkNTPServers", "RuntimeNTPServers" and "Received" (the last answer's
time, seconds since the epoch; 0 for none) set the properties;
"runtime": false is a timesyncd older than systemd 255, without
RuntimeNTPServers and SetRuntimeNTPServers.

The mock method Answer(server) is the server's answer arriving: it
becomes ServerName and NTPMessage says it was received now.
"""

# SPDX-License-Identifier: MIT

import time

import dbus

from dbusmock import MOCK_IFACE

BUS_NAME = "org.freedesktop.timesync1"
MAIN_OBJ = "/org/freedesktop/timesync1"
MAIN_IFACE = "org.freedesktop.timesync1.Manager"
SYSTEM_BUS = True


def ntp_message(received):
    """NTPMessage (uuuuittayttttbtt): leap, version, mode, stratum,
    precision, root delay and dispersion, reference, then the originate,
    receive, transmit and destination timestamps, ignored, packet count
    and jitter."""
    return dbus.Struct(
        (dbus.UInt32(0), dbus.UInt32(4), dbus.UInt32(4), dbus.UInt32(2), dbus.Int32(-23),
         dbus.UInt64(1000), dbus.UInt64(2000), dbus.Array(b"GPS\0", signature="y"),
         dbus.UInt64(received), dbus.UInt64(received), dbus.UInt64(received),
         dbus.UInt64(received), False, dbus.UInt64(1 if received else 0), dbus.UInt64(0)),
        signature="uuuuittayttttbtt",
    )  # fmt: skip


def strv(parameters, name, default):
    return dbus.Array(parameters.get(name, default), signature="s")


def load(mock, parameters):
    props = {
        "LinkNTPServers": strv(parameters, "LinkNTPServers", []),
        "SystemNTPServers": strv(parameters, "SystemNTPServers", []),
        "FallbackNTPServers": strv(parameters, "FallbackNTPServers",
                                   ["0.arch.pool.ntp.org", "1.arch.pool.ntp.org"]),
        "ServerName": parameters.get("ServerName", "0.arch.pool.ntp.org"),
        "ServerAddress": dbus.Struct((dbus.Int32(2), dbus.Array(b"\xc0\x00\x02\x01",
                                                                signature="y")),
                                     signature="iay"),
        "RootDistanceMaxUSec": dbus.UInt64(5000000),
        "PollIntervalMinUSec": dbus.UInt64(32000000),
        "PollIntervalMaxUSec": dbus.UInt64(2048000000),
        "PollIntervalUSec": dbus.UInt64(parameters.get("PollIntervalUSec", 2048000000)),
        "NTPMessage": ntp_message(int(parameters.get("Received", 0) * 1000000)),
        "Frequency": dbus.Int64(0),
    }  # fmt: skip
    if parameters.get("runtime", True):
        props["RuntimeNTPServers"] = strv(parameters, "RuntimeNTPServers", [])
        mock.AddMethod(MAIN_IFACE, "SetRuntimeNTPServers", "as", "",
                       'self.UpdateProperties("%s", {"RuntimeNTPServers": '
                       'dbus.Array(args[0], signature="s"), "ServerName": ""})' % MAIN_IFACE)
    mock.AddProperties(MAIN_IFACE, props)


@dbus.service.method(MOCK_IFACE, in_signature="s", out_signature="")
def Answer(self, server):
    received = int(time.time() * 1000000)
    self.UpdateProperties(MAIN_IFACE, {"ServerName": server, "NTPMessage": ntp_message(received)})
