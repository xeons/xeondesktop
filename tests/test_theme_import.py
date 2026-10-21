#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unit tests for tools/xd-theme-import's reading of XP's DirectUI
stylesheets (shellstyle.dll) into shell/NAME.xml. The stylesheets here are
our own, written in the format, not Windows'.

Exit status 77 (skipped) without pefile or PyGObject, which the importer
needs.
"""
import importlib.machinery
import importlib.util
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IMPORTER = os.path.join(HERE, "..", "tools", "xd-theme-import")


def load_importer():
    sys.dont_write_bytecode = True  # no __pycache__ in the source tree
    loader = importlib.machinery.SourceFileLoader("xd_theme_import", IMPORTER)
    spec = importlib.util.spec_from_loader("xd_theme_import", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


try:
    import gi  # noqa: F401
    import pefile  # noqa: F401

    imp = load_importer()
except (ImportError, ValueError, SystemExit) as e:
    print(f"skipped: the importer's modules are missing ({e})")
    sys.exit(77)

SHEET = """<style resid=sheet>
widget [id=atom(box)][hot]
{
color: rgb(1,2,3);
size: rcint(7)pt;
pad: rect (4rp, 5rp,6rp,7rp);
}
widget
{
flags: one | two;
}
</style>
"""

STRINGS = {1: "Verdana", 7: "9", 8: "700", 9: "0x10"}


class Tokens(unittest.TestCase):
    def kinds(self, text):
        return [(k, v) for k, v, _s, _e in imp.dui_tokens(text)]

    def test_style_tags(self):
        self.assertEqual(self.kinds("<style resid=abc></style>"),
                         [("open", "abc"), ("close", "</style>")])
        self.assertEqual(self.kinds('< style resid="q" >'), [("open", "q")])

    def test_values(self):
        self.assertEqual(self.kinds("x: 12rp -3 1.5 #FF00FF;"),
                         [("ident", "x"), ("punct", ":"), ("number", (12, "rp")),
                          ("number", (-3, None)), ("number", (1.5, None)),
                          ("hash", "#FF00FF"), ("punct", ";")])

    def test_unexpected(self):
        with self.assertRaises(imp.DuiError) as e:
            list(imp.dui_tokens("a {\nb: 'quoted';\n}"))
        self.assertIn("line 2", str(e.exception))


class Parser(unittest.TestCase):
    def test_rules(self):
        sheets = imp.parse_dui(SHEET)
        self.assertEqual(list(sheets), ["sheet"])
        first, second = sheets["sheet"]
        self.assertEqual(first.element, "widget")
        self.assertEqual(set(first.conds), {"id", "hot"})
        self.assertIsNone(first.conds["hot"])
        self.assertEqual(first.conds["id"], [("call", "atom", [[("ident", "box")]], None)])
        # A unit straight after a call's parenthesis belongs to it.
        self.assertEqual(first.decls["size"], [("call", "rcint", [[("num", 7, None)]], "pt")])
        self.assertEqual(second.decls["flags"], [("ident", "one"), ("op", "|"), ("ident", "two")])

    def test_errors(self):
        for bad in ("<style resid=a>\nwidget { color: ; \n", "widget { }",
                    "<style resid=a>\n{ x: 1; }\n</style>"):
            with self.assertRaises(imp.DuiError):
                imp.parse_dui(bad)


class Values(unittest.TestCase):
    def values(self, text=SHEET):
        return imp.DuiValues(imp.parse_dui(text), STRINGS.get)

    def test_rules_by_id_and_flags(self):
        v = self.values()
        self.assertIn("color", v.rules("sheet", "widget", "box", ("hot",)))
        self.assertEqual(v.rules("sheet", "widget", "box"), {})
        self.assertIn("flags", v.rules("sheet", "widget"))

    def test_conversions(self):
        v = self.values()
        d = v.rules("sheet", "widget", "box", ("hot",))
        self.assertEqual(v.color(d["color"]), (1, 2, 3, 255))
        self.assertEqual(v.number(d["size"]), 9)
        self.assertEqual(v.rect(d["pad"]), (4, 5, 6, 7))
        self.assertEqual(imp.margins((4, 5, 6, 7)), "4 6 5 7")
        one = imp.parse_dui("<style resid=s>e { a: argb(0,9,8,7); b: white; c: #102030;"
                            " d: gradient(rgb(1,1,1), argb(0,2,2,2), 1); f: rcstr(1);"
                            " g: rcint(9); h: rcbmp(5,6,#FF00FF,0,0,1,0); i: rcbmp(6,2,255);"
                            " }</style>")
        v = imp.DuiValues(one, STRINGS.get)
        d = v.rules("s", "e")
        self.assertEqual(v.color(d["a"]), (9, 8, 7, 0))
        self.assertEqual(v.color(d["b"]), (255, 255, 255, 255))
        self.assertEqual(v.color(d["c"]), (0x10, 0x20, 0x30, 255))
        self.assertEqual(v.gradient(d["d"]), ((1, 1, 1), (2, 2, 2), True))
        self.assertEqual(v.text(d["f"]), "Verdana")
        self.assertEqual(v.number(d["g"]), 16)
        self.assertEqual(v.bitmap(d["h"]), (5, (255, 0, 255)))
        self.assertEqual(v.bitmap(d["i"]), (6, None))


TASK_PANE = """<style resid=taskpane>
element [id=atom(sectionlist)]
{
background: gradient(argb(0,10,20,30), argb(0,40,50,60), 1);
padding: rect(12rp,12rp,12rp,12rp);
borderthickness: rect(1,0,0,1);
bordercolor: white;
}
</style>
<style resid=sectionss>
expando { fontface: rcstr(1); fontsize: rcint(7)pt; margin: rect(0rp,15rp,0rp,15rp); }
button
{
background: rcbmp(20,6,#FF00FF,0,0,1,0);
foreground: rgb(1,2,3);
fontweight: rcint(8);
padding: rect(10rp,0rp,1rp,0rp);
borderthickness: rect(2,2,2,0);
}
element [id=atom(title)][mousefocused] { foreground: rgb(4,5,6); }
element [id=atom(arrow)] { content: rcbmp(21,2,255,0,0,0,0); }
element [id=atom(arrow)][selected] { content: rcbmp(22,2,255,0,0,0,0); }
tasklist [id=atom(tasklist)]
{
background: rgb(7,8,9);
borderthickness: rect(1,0,1,1);
bordercolor: white;
padding: rect(12rp,10rp,12rp,10rp);
}
</style>
<style resid=sectiontaskss>
actiontask { margin: rect(0rp,4rp,0rp,0rp); }
button { foreground: rgb(11,12,13); }
button [mousefocused] { fontstyle: underline; }
element [id=atom(title)] { padding: rect(6rp,0rp,4rp,0rp); }
element [id=atom(title)][mousefocused] { foreground: rgb(14,15,16); }
</style>
<style resid=NameSpaceItemInfoList>
NameSpaceItemInfo { margin: rect(0rp,5rp,0rp,0rp); }
NameSpaceItemInfo [id=atom(InfoName)] { fontweight: rcint(8); }
</style>
"""


class Shell(unittest.TestCase):
    def test_task_pane(self):
        made = []

        def bitmap(ref):
            made.append(ref)
            return f"images/{ref[0]}.png"

        root = imp.shell_root(imp.DuiValues(imp.parse_dui(TASK_PANE), STRINGS.get), bitmap)
        parts = {}
        for cls in root.iter("class"):
            for p in cls.iter("part"):
                key = (cls.get("name"), p.get("name"), p.get("state"))
                parts.setdefault(key, {}).update(p.attrib)
        pane = parts[("ExplorerBar", None, None)]
        self.assertEqual(pane["FillType"], "VertGradient")
        self.assertEqual((pane["GradientColor1"], pane["GradientColor2"]), ("10 20 30", "40 50 60"))
        self.assertEqual(pane["BorderThickness"], "1 0 0 1")
        self.assertEqual(pane["ContentMargins"], "13 12 12 13")
        self.assertEqual(pane["GroupSpacing"], "15")
        head = parts[("ExplorerBar", "NormalGroupHead", None)]
        self.assertEqual(head["ImageFile"], "images/20.png")
        self.assertEqual(head["SizingMargins"], "2 2 2 0")
        self.assertEqual(head["ContentMargins"], "12 3 2 0")
        self.assertEqual(head["Font"], "Verdana, 9, Bold")
        self.assertEqual(head["TextColor"], "1 2 3")
        self.assertEqual(parts[("ExplorerBar", "NormalGroupHead", "Hot")]["TextColor"], "4 5 6")
        # The arrow: shown expanded ("selected") is Collapse.
        self.assertEqual(parts[("ExplorerBar", "NormalGroupExpand", "Normal")]["ImageFile"],
                         "images/21.png")
        collapse = parts[("ExplorerBar", "NormalGroupCollapse", "Hot")]
        self.assertEqual(collapse["ImageFile"], "images/22.png")
        self.assertEqual(collapse["Offset"], "-3 2")
        body = parts[("ExplorerBar", "NormalGroupBackground", None)]
        self.assertEqual(body["FillColor"], "7 8 9")
        self.assertEqual(body["BorderThickness"], "1 1 0 1")
        self.assertEqual(body["ContentMargins"], "13 13 10 11")
        self.assertEqual(body["TextColor"], "11 12 13")
        self.assertEqual(body["Font"], "Verdana, 9")
        self.assertEqual((body["LinkSpacing"], body["IconSpacing"]), ("4", "6"))
        hot = parts[("ExplorerBar", "NormalGroupBackground", "Hot")]
        self.assertEqual((hot["TextColor"], hot["Font"]), ("14 15 16", "Verdana, 9, Underline"))
        self.assertEqual(parts[("TaskPane", "Details", None)]["LinkSpacing"], "5")
        self.assertEqual(parts[("TaskPane", "DetailsName", None)]["Font"], "Verdana, 9, Bold")
        # The magenta of the title bar's bitmap is its transparent colour.
        self.assertIn((20, (255, 0, 255)), made)
        self.assertIn((21, None), made)


THEME = r"""[Theme]
DisplayName=Test Sounds

[Control Panel\Desktop]
Wallpaper=%ThemeDir%Pictures\Sky.png
TileWallpaper=0
WallpaperStyle=10

[AppEvents\Schemes\Apps\.Default\WindowsLogon\.Current]
DefaultValue=%ThemeDir%Sounds\Hello.wav

[AppEvents\Schemes\Apps\.Default\SystemHand\.Current]
DefaultValue="%ThemeDir%Sounds\Stop.wav"

[AppEvents\Schemes\Apps\.Default\Maximize\.Current]
DefaultValue=

[AppEvents\Schemes\Apps\.Default\SystemAsterisk\.Current]
DefaultValue=%SystemRoot%\Media\Windows XP Ding.wav

[AppEvents\Schemes\Apps\Explorer\EmptyRecycleBin\.Current]
DefaultValue=%ThemeDir%Sounds\Hello.wav

[AppEvents\Schemes\Apps\.Default\Navigating\.Current]
DefaultValue=%ThemeDir%Sounds\Hello.wav

[Control Panel\Cursors]
Arrow=%ThemeDir%Pointers\arrow.cur
Wait=%ThemeDir%Pointers\busy.ani
IBeam=
Hand=%SystemRoot%\cursors\aero_link.cur
DefaultValue=Test pointers
"""


class ThemeExtras(unittest.TestCase):
    """A .theme's sounds and pointers (our own files, beside it)."""

    def setUp(self):
        import tempfile

        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        for rel, data in (("Pictures/Sky.png", b"png"), ("Sounds/Hello.wav", b"RIFFhello"),
                          ("Sounds/Stop.wav", b"RIFFstop"), ("Pointers/arrow.cur", b"cur"),
                          ("Pointers/busy.ani", b"ani")):
            os.makedirs(os.path.dirname(os.path.join(self.dir, rel)), exist_ok=True)
            with open(os.path.join(self.dir, rel), "wb") as f:
                f.write(data)
        path = os.path.join(self.dir, "test.theme")
        with open(path, "w", encoding="cp1252") as f:
            f.write(THEME)
        self.cp = imp.read_theme_file(path)

    def tearDown(self):
        self.tmp.cleanup()

    def test_sounds(self):
        sounds = imp.theme_sounds(self.cp, self.dir)
        hello = os.path.join(self.dir, "Sounds", "Hello.wav")
        # Windows' own Media isn't beside it (Asterisk keeps the default);
        # Explorer's events have theirs; Navigating has none of ours.
        self.assertEqual(sounds, {"WindowsLogon": hello, "EmptyRecycleBin": hello,
                                  "CriticalStop": os.path.join(self.dir, "Sounds", "Stop.wav"),
                                  "Maximize": ""})
        pkg = imp.Package()
        self.assertEqual(imp.import_sounds(pkg, sounds), "sounds/scheme.ini")
        self.assertEqual(pkg.files["sounds/Hello.wav"], b"RIFFhello")
        self.assertEqual(pkg.files["sounds/Stop.wav"], b"RIFFstop")
        scheme = pkg.files["sounds/scheme.ini"].decode()
        self.assertIn("[Sounds]\nScheme=default\n", scheme)
        for line in ("WindowsLogon=sounds/Hello.wav", "CriticalStop=sounds/Stop.wav",
                     "EmptyRecycleBin=sounds/Hello.wav", "Maximize=\n"):
            self.assertIn(line, scheme)
        self.assertNotIn("Asterisk", scheme)
        self.assertIsNone(imp.import_sounds(imp.Package(), {}))

    def test_cursors(self):
        cursors = imp.theme_cursors(self.cp, self.dir)
        self.assertEqual(cursors, {"arrow": os.path.join(self.dir, "Pointers", "arrow.cur"),
                                   "wait": os.path.join(self.dir, "Pointers", "busy.ani")})
        pkg = imp.Package()
        self.assertEqual(imp.import_cursors(pkg, cursors, "Test", lambda b: b"XC" + b), "cursors")
        for name in ("left_ptr", "default", "arrow"):
            self.assertEqual(pkg.files["cursors/cursors/" + name], b"XCcur")
        for name in ("wait", "watch"):
            self.assertEqual(pkg.files["cursors/cursors/" + name], b"XCani")
        self.assertIn(b"Name=Test\nInherits=default", pkg.files["cursors/index.theme"])
        # No converter (win2xcur missing): no pointers, rather than broken ones.
        pkg = imp.Package()
        self.assertIsNone(imp.import_cursors(pkg, cursors, "Test", lambda b: None))
        self.assertEqual(pkg.files, {})

    def test_extras(self):
        pkg = imp.Package()
        extras = imp.import_theme_extras(self.cp, self.dir, pkg, "Test", lambda b: b"XC")
        self.assertEqual(extras, [("wallpaper", {"file": "wallpaper/Sky.png", "style": "fill"}),
                                  ("sounds", {"scheme": "sounds/scheme.ini"}),
                                  ("cursors", {"theme": "cursors"})])
        self.assertEqual(pkg.files["wallpaper/Sky.png"], b"png")


if __name__ == "__main__":
    unittest.main()
