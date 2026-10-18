"""AccountsService (org.freedesktop.Accounts) for python-dbusmock, which
has no template of its own in every version: the manager and user objects
xd-users uses. The calling user (an administrator) and "bob" exist;
the parameter "BobHome" gives bob's home folder (default /home/bob).

    python3 -m dbusmock --system -t tests/dbusmock/accounts.py
"""

# SPDX-License-Identifier: MIT

import os
import pwd

import dbus

from dbusmock import mockobject

BUS_NAME = "org.freedesktop.Accounts"
MAIN_OBJ = "/org/freedesktop/Accounts"
MAIN_IFACE = "org.freedesktop.Accounts"
USER_IFACE = "org.freedesktop.Accounts.User"
SYSTEM_BUS = True


def user_path(uid):
    return f"/org/freedesktop/Accounts/User{uid}"


def setter(prop, extra=""):
    return f'self.Set("{USER_IFACE}", "{prop}", args[0]); {extra}self.EmitSignal("{USER_IFACE}", "Changed", "", [])'


def add_user(mock, uid, name, real_name, account_type, home=None):
    path = user_path(uid)
    mock.AddObject(
        path,
        USER_IFACE,
        {
            "Uid": dbus.UInt64(uid),
            "UserName": name,
            "RealName": real_name,
            "AccountType": dbus.Int32(account_type),
            "HomeDirectory": home or f"/home/{name}",
            "Shell": "/bin/bash",
            "Email": "",
            "Language": "",
            "IconFile": "",
            "Locked": False,
            "PasswordMode": dbus.Int32(0),
            "PasswordHint": "",
            "AutomaticLogin": False,
            "SystemAccount": False,
            "LocalAccount": True,
        },
        [
            ("SetRealName", "s", "", setter("RealName")),
            ("SetIconFile", "s", "", setter("IconFile")),
            ("SetAccountType", "i", "", setter("AccountType")),
            ("SetLocked", "b", "", setter("Locked")),
            ("SetPasswordMode", "i", "", setter("PasswordMode")),
            (
                "SetPassword",
                "ss",
                "",
                f'self.Set("{USER_IFACE}", "PasswordHint", args[1]); '
                f'self.Set("{USER_IFACE}", "PasswordMode", dbus.Int32(0)); '
                f'self.EmitSignal("{USER_IFACE}", "Changed", "", [])',
            ),
        ],
    )
    return dbus.ObjectPath(path)


def load(mock, parameters):
    mock.next_uid = 2000
    me = pwd.getpwuid(os.getuid()).pw_name
    add_user(mock, os.getuid(), me, parameters.get("RealName", "Test User"), 1,
             pwd.getpwuid(os.getuid()).pw_dir)
    add_user(mock, 1999, "bob", "Bob", 0, parameters.get("BobHome"))
    mock.AddProperties(
        MAIN_IFACE,
        dbus.Dictionary(
            {"DaemonVersion": "mock", "HasNoUsers": False, "HasMultipleUsers": True,
             "AutomaticLoginUsers": dbus.Array([], signature="o")},
            signature="sv",
        ),
    )


def users():
    return [p for p, o in mockobject.objects.items() if p.startswith("/org/freedesktop/Accounts/User")]


def find(prop, value):
    for p in users():
        if mockobject.objects[p].Get(USER_IFACE, prop) == value:
            return dbus.ObjectPath(p)
    raise dbus.exceptions.DBusException("No such user", name="org.freedesktop.Accounts.Error.Failed")


@dbus.service.method(MAIN_IFACE, in_signature="", out_signature="ao")
def ListCachedUsers(_):
    return [dbus.ObjectPath(p) for p in sorted(users())]


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def FindUserByName(_, name):
    return find("UserName", name)


@dbus.service.method(MAIN_IFACE, in_signature="x", out_signature="o")
def FindUserById(_, uid):
    return find("Uid", dbus.UInt64(uid))


@dbus.service.method(MAIN_IFACE, in_signature="ssi", out_signature="o")
def CreateUser(self, name, real_name, account_type):
    for p in users():
        if mockobject.objects[p].Get(USER_IFACE, "UserName") == name:
            raise dbus.exceptions.DBusException(
                f"A user with name '{name}' already exists", name="org.freedesktop.Accounts.Error.UserExists"
            )
    uid = self.next_uid
    self.next_uid += 1
    path = add_user(self, uid, name, real_name, account_type)
    self.EmitSignal(MAIN_IFACE, "UserAdded", "o", [path])
    return path


@dbus.service.method(MAIN_IFACE, in_signature="xb", out_signature="")
def DeleteUser(self, uid, _remove_files):
    path = find("Uid", dbus.UInt64(uid))
    self.RemoveObject(path)
    self.EmitSignal(MAIN_IFACE, "UserDeleted", "o", [path])
