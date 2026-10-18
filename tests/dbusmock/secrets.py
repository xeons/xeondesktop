"""The Secret Service (org.freedesktop.secrets, as gnome-keyring and
KeePassXC offer it) for python-dbusmock, which has no template of its own:
what libsecret uses for Passwords and Keys (xd-keys). Plain sessions only
(libsecret falls back to them when the encrypted one is refused).

Two collections: "Login" (the default alias, unlocked) with a network
password and a stored note, and "Work" (locked) with one password.
Unlocking goes through a prompt, as with gnome-keyring, which completes
at once.

    python3 -m dbusmock -t tests/dbusmock/secrets.py
"""

# SPDX-License-Identifier: MIT

import time

import dbus
from gi.repository import GLib

from dbusmock import mockobject

BUS_NAME = "org.freedesktop.secrets"
MAIN_OBJ = "/org/freedesktop/secrets"
MAIN_IFACE = "org.freedesktop.Secret.Service"
SYSTEM_BUS = False

COLLECTION_IFACE = "org.freedesktop.Secret.Collection"
ITEM_IFACE = "org.freedesktop.Secret.Item"
PROMPT_IFACE = "org.freedesktop.Secret.Prompt"
SESSION_IFACE = "org.freedesktop.Secret.Session"
COLLECTION_BASE = "/org/freedesktop/secrets/collection/"

# What's not on the bus: each item's secret (bytes, content type).
secrets = {}
counter = [0]
# The Service object (not in mockobject.objects while load() runs).
service = []


def error(name, text):
    return dbus.exceptions.DBusException(text, name="org.freedesktop.Secret.Error." + name)


def main():
    return service[0]


def next_id():
    counter[0] += 1
    return counter[0]


def is_collection(path):
    return path.startswith(COLLECTION_BASE) and path.count("/") == 5


def collection_of(item_path):
    return item_path.rsplit("/", 1)[0]


def items_of(collection_path):
    return [dbus.ObjectPath(p) for p in sorted(mockobject.objects)
            if p.startswith(collection_path + "/") and p.count("/") == 6]  # fmt: skip


def locked(path):
    c = collection_of(path) if not is_collection(path) else path
    return bool(mockobject.objects[c].Get(COLLECTION_IFACE, "Locked"))


def now():
    return dbus.UInt64(int(time.time()))


def secret_struct(session, path):
    value, content_type = secrets[path]
    return dbus.Struct(
        (dbus.ObjectPath(session), dbus.ByteArray(b""), dbus.ByteArray(value), content_type),
        signature="oayays",
    )


def item_get_secret(self, session):
    if locked(self.__dbus_object_path__):
        raise error("IsLocked", "The item is locked")
    return secret_struct(session, self.__dbus_object_path__)


def item_set_secret(self, secret):
    if locked(self.__dbus_object_path__):
        raise error("IsLocked", "The item is locked")
    secrets[self.__dbus_object_path__] = (bytes(secret[2]), str(secret[3]))
    self.UpdateProperties(ITEM_IFACE, {"Modified": now()})


def item_delete(self):
    path = self.__dbus_object_path__
    if locked(path):
        raise error("IsLocked", "The item is locked")
    coll = collection_of(path)
    secrets.pop(path, None)
    main().RemoveObject(path)
    c = mockobject.objects[coll]
    c.UpdateProperties(COLLECTION_IFACE, {"Items": dbus.Array(items_of(coll), signature="o")})
    c.EmitSignal(COLLECTION_IFACE, "ItemDeleted", "o", [dbus.ObjectPath(path)])
    return dbus.ObjectPath("/")


def matches(path, attributes):
    have = mockobject.objects[path].Get(ITEM_IFACE, "Attributes")
    return all(have.get(k) == v for k, v in attributes.items())


def add_item(coll, label, attributes, value, content_type="text/plain"):
    path = f"{coll}/{next_id()}"
    t = now()
    main().AddObject(
        path,
        ITEM_IFACE,
        {
            "Label": dbus.String(label),
            "Attributes": dbus.Dictionary(attributes, signature="ss"),
            "Locked": dbus.Boolean(locked(coll)),
            "Created": t,
            "Modified": t,
        },
        [],
    )
    obj = mockobject.objects[path]
    obj.AddMethod(ITEM_IFACE, "GetSecret", "o", "(oayays)", item_get_secret)
    obj.AddMethod(ITEM_IFACE, "SetSecret", "(oayays)", "", item_set_secret)
    obj.AddMethod(ITEM_IFACE, "Delete", "", "o", item_delete)
    secrets[path] = (value, content_type)
    return dbus.ObjectPath(path)


def collection_create_item(self, properties, secret, replace):
    coll = self.__dbus_object_path__
    if locked(coll):
        raise error("IsLocked", "The collection is locked")
    label = properties.get("org.freedesktop.Secret.Item.Label", "")
    attributes = dict(properties.get("org.freedesktop.Secret.Item.Attributes", {}))
    if replace:
        for p in items_of(coll):
            if mockobject.objects[p].Get(ITEM_IFACE, "Attributes") == attributes:
                secrets[p] = (bytes(secret[2]), str(secret[3]))
                mockobject.objects[p].UpdateProperties(ITEM_IFACE, {"Label": label})
                return (p, dbus.ObjectPath("/"))
    path = add_item(coll, label, attributes, bytes(secret[2]), str(secret[3]))
    self.UpdateProperties(COLLECTION_IFACE, {"Items": dbus.Array(items_of(coll), signature="o")})
    self.EmitSignal(COLLECTION_IFACE, "ItemCreated", "o", [path])
    return (path, dbus.ObjectPath("/"))


def collection_search_items(self, attributes):
    return dbus.Array([p for p in items_of(self.__dbus_object_path__) if matches(p, attributes)],
                      signature="o")  # fmt: skip


def collection_delete(self):
    raise error("NotSupported", "The mock keeps its collections")


def add_collection(name, label, is_locked):
    path = COLLECTION_BASE + name
    t = now()
    main().AddObject(
        path,
        COLLECTION_IFACE,
        {
            "Label": dbus.String(label),
            "Locked": dbus.Boolean(is_locked),
            "Created": t,
            "Modified": t,
            "Items": dbus.Array([], signature="o"),
        },
        [],
    )
    obj = mockobject.objects[path]
    obj.AddMethod(COLLECTION_IFACE, "CreateItem", "a{sv}(oayays)b", "oo", collection_create_item)
    obj.AddMethod(COLLECTION_IFACE, "SearchItems", "a{ss}", "ao", collection_search_items)
    obj.AddMethod(COLLECTION_IFACE, "Delete", "", "o", collection_delete)
    return path


def set_locked(paths, value):
    """Locks or unlocks the collections among paths (an item counts as its
    collection); returns the paths it covered."""
    done = []
    for p in paths:
        coll = p if is_collection(p) else collection_of(p)
        if coll not in mockobject.objects:
            continue
        c = mockobject.objects[coll]
        if bool(c.Get(COLLECTION_IFACE, "Locked")) != value:
            c.UpdateProperties(COLLECTION_IFACE, {"Locked": dbus.Boolean(value)})
            for i in items_of(coll):
                mockobject.objects[i].UpdateProperties(ITEM_IFACE, {"Locked": dbus.Boolean(value)})
        done.append(dbus.ObjectPath(p))
    return done


def load(mock, _parameters):
    service.append(mock)
    mock.AddProperties(MAIN_IFACE, {"Collections": dbus.Array([], signature="o")})
    login = add_collection("login", "Login", False)
    add_item(
        login,
        "Mail server password",
        {"xdg:schema": "org.gnome.keyring.NetworkPassword", "user": "alice",
         "server": "mail.example.com", "protocol": "imap"},  # fmt: skip
        b"hunter2",
    )
    add_item(login, "Alarm code", {"xdg:schema": "org.gnome.keyring.Note"}, b"1234")
    work = add_collection("work", "Work", True)
    add_item(work, "VPN", {"xdg:schema": "org.gnome.keyring.NetworkPassword",
                           "server": "vpn.example.com"}, b"s3cret")  # fmt: skip
    for c in (login, work):
        mockobject.objects[c].UpdateProperties(
            COLLECTION_IFACE, {"Items": dbus.Array(items_of(c), signature="o")}
        )
    mock.UpdateProperties(
        MAIN_IFACE, {"Collections": dbus.Array([dbus.ObjectPath(login), dbus.ObjectPath(work)],
                                               signature="o")}  # fmt: skip
    )
    mock.aliases = {"default": login, "login": login}
    # gnome-keyring's alias objects: libsecret stores into "default" there.
    alias = "/org/freedesktop/secrets/aliases/default"
    mock.AddObject(alias, COLLECTION_IFACE, {}, [])
    mockobject.objects[alias].AddMethod(
        COLLECTION_IFACE, "CreateItem", "a{sv}(oayays)b", "oo",
        lambda _self, *args: collection_create_item(mockobject.objects[mock.aliases["default"]], *args),
    )  # fmt: skip


@dbus.service.method(MAIN_IFACE, in_signature="sv", out_signature="vo")
def OpenSession(self, algorithm, _input):
    if algorithm != "plain":
        raise dbus.exceptions.DBusException(
            "Only plain sessions", name="org.freedesktop.DBus.Error.NotSupported"
        )
    path = f"/org/freedesktop/secrets/session/{next_id()}"
    self.AddObject(path, SESSION_IFACE, {}, [("Close", "", "", "")])
    return (dbus.String("", variant_level=1), dbus.ObjectPath(path))


@dbus.service.method(MAIN_IFACE, in_signature="a{ss}", out_signature="aoao")
def SearchItems(_self, attributes):
    unlocked, locked_ = [], []
    for c in [p for p in mockobject.objects if is_collection(p)]:
        for p in items_of(c):
            if matches(p, attributes):
                (locked_ if locked(p) else unlocked).append(p)
    return (dbus.Array(unlocked, signature="o"), dbus.Array(locked_, signature="o"))


@dbus.service.method(MAIN_IFACE, in_signature="aoo", out_signature="a{o(oayays)}")
def GetSecrets(_self, items, session):
    out = {}
    for p in items:
        if p in secrets and not locked(p):
            out[p] = secret_struct(session, p)
    return dbus.Dictionary(out, signature="o(oayays)")


@dbus.service.method(MAIN_IFACE, in_signature="ao", out_signature="aoo")
def Lock(_self, objects):
    return (dbus.Array(set_locked(objects, True), signature="o"), dbus.ObjectPath("/"))


@dbus.service.method(MAIN_IFACE, in_signature="ao", out_signature="aoo")
def Unlock(self, objects):
    """What's unlocked already comes back at once; the rest through a
    prompt (gnome-keyring asks for the keyring's password there)."""
    ready = [dbus.ObjectPath(p) for p in objects if p in mockobject.objects and not locked(p)]
    waiting = [p for p in objects if p in mockobject.objects and locked(p)]
    if not waiting:
        return (dbus.Array(ready, signature="o"), dbus.ObjectPath("/"))
    path = f"/org/freedesktop/secrets/prompt/{next_id()}"
    self.AddObject(path, PROMPT_IFACE, {}, [])
    prompt = mockobject.objects[path]

    def complete():
        done = set_locked(waiting, False)
        prompt.EmitSignal(PROMPT_IFACE, "Completed", "bv",
                          [False, dbus.Array(done, signature="o", variant_level=1)])  # fmt: skip
        return False

    def do_prompt(_obj, _window):
        GLib.timeout_add(50, complete)

    def dismiss(_obj):
        prompt.EmitSignal(PROMPT_IFACE, "Completed", "bv",
                          [True, dbus.Array([], signature="o", variant_level=1)])  # fmt: skip

    prompt.AddMethod(PROMPT_IFACE, "Prompt", "s", "", do_prompt)
    prompt.AddMethod(PROMPT_IFACE, "Dismiss", "", "", dismiss)
    return (dbus.Array(ready, signature="o"), dbus.ObjectPath(path))


@dbus.service.method(MAIN_IFACE, in_signature="s", out_signature="o")
def ReadAlias(self, name):
    return dbus.ObjectPath(self.aliases.get(name, "/"))


@dbus.service.method(MAIN_IFACE, in_signature="so", out_signature="")
def SetAlias(self, name, collection):
    self.aliases[name] = collection


@dbus.service.method(MAIN_IFACE, in_signature="a{sv}s", out_signature="oo")
def CreateCollection(self, properties, alias):
    """As gnome-keyring: an alias that exists already gives its collection
    (libsecret stores into "default" this way)."""
    if alias and alias in self.aliases:
        return (dbus.ObjectPath(self.aliases[alias]), dbus.ObjectPath("/"))
    label = str(properties.get("org.freedesktop.Secret.Collection.Label", "Untitled"))
    path = add_collection(f"c{next_id()}", label, False)
    if alias:
        self.aliases[alias] = path
    colls = [p for p in sorted(mockobject.objects) if is_collection(p)]
    self.UpdateProperties(MAIN_IFACE, {"Collections": dbus.Array(colls, signature="o")})
    self.EmitSignal(MAIN_IFACE, "CollectionCreated", "o", [dbus.ObjectPath(path)])
    return (dbus.ObjectPath(path), dbus.ObjectPath("/"))
