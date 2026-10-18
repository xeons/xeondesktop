"""A minimal Secret Service (org.freedesktop.secrets) for python-dbusmock,
which has no template of its own: enough of the API for libsecret's
secret_password_store, _lookup and _clear, as xd-files remembers LUKS
passphrases with. Nothing here touches a real keyring.

    python3 -m dbusmock -t tests/dbusmock/luks_secrets.py

One collection, "login" (the "default" alias), always unlocked; sessions
are "plain" only (libsecret falls back to it when "dh" is refused), so
the secrets travel as they are. The mock method MockItems lists what's
stored: (label, attributes, secret) each.
"""

# SPDX-License-Identifier: MIT

import types

import dbus

from dbusmock import MOCK_IFACE, mockobject

BUS_NAME = "org.freedesktop.secrets"
MAIN_OBJ = "/org/freedesktop/secrets"
MAIN_IFACE = "org.freedesktop.Secret.Service"
SYSTEM_BUS = False

S = "org.freedesktop.Secret"
COLLECTION = S + ".Collection"
ITEM = S + ".Item"
SESSION = S + ".Session"
LOGIN = MAIN_OBJ + "/collection/login"

state = types.SimpleNamespace(main=None, items={}, next_item=1, next_session=1)


def no_prompt():
    return dbus.ObjectPath("/")


def obj(path):
    return mockobject.objects[str(path)]


def matches(path, attrs):
    have = state.items[path]["attributes"]
    return all(have.get(str(k)) == str(v) for k, v in attrs.items())


def search(attrs):
    return dbus.Array([dbus.ObjectPath(p) for p in state.items if matches(p, attrs)],
                      signature="o")


def secret_of(path, session):
    return dbus.Struct((dbus.ObjectPath(session), dbus.ByteArray(b""),
                        dbus.ByteArray(state.items[path]["secret"]), state.items[path]["type"]),
                       signature="oayays")


def collection_items():
    obj(LOGIN).UpdateProperties(COLLECTION, {
        "Items": dbus.Array([dbus.ObjectPath(p) for p in state.items], signature="o")})


def load(mock, _parameters):
    state.main = mock
    state.items = {}
    mockobject.xd_secrets = types.SimpleNamespace(
        open_session=open_session, search=search, get_secrets=get_secrets,
        create_item=create_item, delete=delete_item, get_secret=secret_of)
    mock.AddProperties(MAIN_IFACE, dbus.Dictionary({
        "Collections": dbus.Array([dbus.ObjectPath(LOGIN)], signature="o")}, signature="sv"))
    mock.AddMethods(MAIN_IFACE, [
        ("OpenSession", "sv", "vo", "ret = xd_secrets.open_session(args[0])"),
        ("SearchItems", "a{ss}", "aoao",
         "ret = (xd_secrets.search(args[0]), dbus.Array([], signature='o'))"),
        ("Unlock", "ao", "aoo", "ret = (dbus.Array(args[0], signature='o'), dbus.ObjectPath('/'))"),
        ("Lock", "ao", "aoo", "ret = (dbus.Array([], signature='o'), dbus.ObjectPath('/'))"),
        ("GetSecrets", "aoo", "a{o(oayays)}", "ret = xd_secrets.get_secrets(args[0], args[1])"),
        ("ReadAlias", "s", "o",
         f"ret = dbus.ObjectPath('{LOGIN}' if args[0] == 'default' else '/')"),
        ("SetAlias", "so", "", ""),
        # libsecret makes sure of the "default" collection this way: it's
        # the one there is.
        ("CreateCollection", "a{sv}s", "oo",
         f"ret = (dbus.ObjectPath('{LOGIN}'), dbus.ObjectPath('/'))"),
    ])
    mock.AddMethod(MOCK_IFACE, "MockItems", "", "a(sa{ss}s)",
                   "ret = [(i['label'], i['attributes'], bytes(i['secret']).decode()) "
                   "for i in xd_secrets.items.values()]")
    mockobject.xd_secrets.items = state.items
    # libsecret stores through the alias's own path.
    for path in (LOGIN, MAIN_OBJ + "/aliases/default"):
        mock.AddObject(path, COLLECTION, {
            "Items": dbus.Array([], signature="o"), "Label": "Login", "Locked": False,
            "Created": dbus.UInt64(0), "Modified": dbus.UInt64(0),
        }, [
            ("CreateItem", "a{sv}(oayays)b", "oo",
             "ret = xd_secrets.create_item(args[0], args[1], args[2])"),
            ("SearchItems", "a{ss}", "ao", "ret = xd_secrets.search(args[0])"),
            ("Delete", "", "o", "ret = dbus.ObjectPath('/')"),
        ])


def open_session(algorithm):
    if algorithm != "plain":
        raise dbus.exceptions.DBusException("Only plain sessions here",
                                            name="org.freedesktop.DBus.Error.NotSupported")
    path = f"{MAIN_OBJ}/session/s{state.next_session}"
    state.next_session += 1
    state.main.AddObject(path, SESSION, {}, [("Close", "", "", "")])
    return (dbus.String("", variant_level=1), dbus.ObjectPath(path))


def get_secrets(items, session):
    return dbus.Dictionary({dbus.ObjectPath(p): secret_of(str(p), session)
                            for p in items if str(p) in state.items}, signature="o(oayays)")


def create_item(props, secret, replace):
    attrs = {str(k): str(v) for k, v in props.get(ITEM + ".Attributes", {}).items()}
    label = str(props.get(ITEM + ".Label", ""))
    path = None
    if replace:
        for p in state.items:
            if state.items[p]["attributes"] == attrs:
                path = p
    if path is None:
        path = f"{LOGIN}/i{state.next_item}"
        state.next_item += 1
        state.main.AddObject(path, ITEM, {
            "Locked": False, "Created": dbus.UInt64(0), "Modified": dbus.UInt64(0),
            "Label": label, "Attributes": dbus.Dictionary(attrs, signature="ss"),
        }, [
            ("Delete", "", "o", "ret = xd_secrets.delete(self.path)"),
            ("GetSecret", "o", "(oayays)", "ret = xd_secrets.get_secret(self.path, args[0])"),
        ])
    state.items[path] = {"label": label, "attributes": attrs,
                         "secret": bytes(bytearray(int(b) for b in secret[2])),
                         "type": str(secret[3])}
    collection_items()
    return (dbus.ObjectPath(path), no_prompt())


def delete_item(path):
    state.items.pop(str(path), None)
    state.main.RemoveObject(str(path))
    collection_items()
    return no_prompt()
