#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measures the stretchable parts of Windows' own visual styles (push
buttons, drop-down and scroll bar buttons, scroll thumbs and tracks, tabs,
tab pages, group boxes, caption buttons) as a few numbers for the
built-in styles to draw them from procedurally:

    tools/controls-measure.py luna PKGS > src/theme/luna_controls.h
    tools/controls-measure.py aero PKGS > src/theme/aero_controls.h

PKGS holds packages imported by xd-theme-import from luna.msstyles and
royale.msstyles (luna/ and royale/) or aero.msstyles (win7/). They are only
read, and no pixels are copied: each part's state becomes a shape
(xd_shape in src/theme/style.h) that luna.c draws at any size:

- its natural size;
- its corner radius;
- up to four lines along each side (shadow, border, rings), each one
  colour or a gradient between two;
- its face, a vertical gradient (its rows' averages) and a horizontal
  one (its columns'), at most six stops each.

Also measured: the colours of check boxes, radio buttons and glyphs.
Colours are premultiplied ARGB. Glyphs are drawn by luna.c and
style_aero.c. Needs ImageMagick.
"""
import subprocess
import sys
import xml.etree.ElementTree as ET

MAX_STOPS = 6  # a gradient's stops at most

# What each style measures: (C name, class, part, lines, states). Lines
# are those along each side; states None: every state.
LUNA_PARTS = [
    ("pushbutton", "Button", "PushButton", 4, None),
    ("dropdown", "ComboBox", "DropDownButton", 2, None),
    ("scrollarrow", "ScrollBar", "ArrowBtn", 3, range(0, 8)),  # up, down
    ("scrollthumb", "ScrollBar", "ThumbBtnVert", 3, None),
    ("scrolltrack", "ScrollBar", "UpperTrackVert", 2, None),
    ("tab", "Tab", "TabItem", 3, None),
    ("tabtop", "Tab", "TopTabItem", 3, None),
    ("tabpane", "Tab", "Pane", 3, None),
    ("groupbox", "Button", "GroupBox", 3, None),
    ("captionbutton", "Window", "MaxButton", 3, None),
    ("closebutton", "Window", "CloseButton", 3, None),
]
LUNA_VARIANTS = [  # (C name, package, style file)
    ("blue", "luna", "default-blue-normal"),
    ("olive", "luna", "olive-green-normal"),
    ("silver", "luna", "silver-normal"),
    ("energy", "royale", "royale-normal"),
]
AERO_PARTS = [
    ("pushbutton", "Button", "PushButton", 3, None),
    ("check", "Button", "CheckBox", 3, range(0, 4)),  # unchecked; the tick is drawn
    ("groupbox", "Button", "GroupBox", 2, None),
    ("field", "Edit", "EditBorder_NoScroll", 2, None),
    ("dropdown", "ComboBox", "DropDownButtonRight", 2, None),
    ("scrollarrow", "ScrollBar", "ArrowBtn", 3, range(0, 8)),
    ("scrollthumb", "ScrollBar", "ThumbBtnVert", 3, None),
    ("scrolltrack", "ScrollBar", "UpperTrackVert", 2, None),
    ("tab", "Tab", "TabItem", 3, None),
    ("tabtop", "Tab", "TopTabItem", 3, None),
    # Glass caption buttons: normal, hot, pressed, disabled.
    ("dwm_min", "DWMWindow", "MinButton", 3, None),
    ("dwm_min_inactive", "DWMWindow", "MinButtonInactive", 3, None),
    ("dwm_max", "DWMWindow", "MaxButton", 3, None),
    ("dwm_max_inactive", "DWMWindow", "MaxButtonInactive", 3, None),
    ("dwm_close", "DWMWindow", "CloseButton", 3, None),
    ("dwm_close_inactive", "DWMWindow", "CloseButtonInactive", 3, None),
    ("dwm_close_alone", "DWMWindow", "CloseButtonAlone", 3, None),
    ("dwm_close_alone_inactive", "DWMWindow", "CloseButtonAloneInactive", 3, None),
    ("dwm_glow", "DWMWindow", "ButtonGlow", 4, None),
    ("dwm_close_glow", "DWMWindow", "CloseGlow", 4, None),
    # Windows 7 Basic: the caption buttons without glass.
    ("basic_button", "Window", "MaxButton", 2, None),
    ("basic_close", "Window", "CloseButton", 3, None),
]
AERO_VARIANTS = [("aero", "win7", "normal")]


_images = {}


def load_png(path):
    if path in _images:
        return _images[path]
    size = subprocess.run(["magick", "identify", "-format", "%w %h", path],
                          capture_output=True, text=True, check=True).stdout.split()
    w, h = int(size[0]), int(size[1])
    raw = subprocess.run(["magick", path, "-depth", "8", "rgba:-"], capture_output=True,
                         check=True).stdout
    _images[path] = [[tuple(raw[(y * w + x) * 4:(y * w + x) * 4 + 4]) for x in range(w)]
                     for y in range(h)]
    return _images[path]


class Style:
    def __init__(self, pkg_dir, style_file):
        self.dir = pkg_dir
        self.root = ET.parse(f"{pkg_dir}/styles/{style_file}.xml").getroot()

    def props(self, cls, part):
        """The part's properties, stateless, over its class's."""
        out = {}
        for c in self.root.iter("class"):
            if c.get("name", "").lower() != cls.lower():
                continue
            for p in c.iter("part"):
                if p.get("state") is not None:
                    continue
                name = p.get("name")
                if name is None or name.lower() == part.lower():
                    for k, v in p.attrib.items():
                        if name is None:
                            out.setdefault(k.lower(), v)
                        else:
                            out[k.lower()] = v
        return out


def premul(p):
    a = p[3]
    return (a, p[0] * a // 255, p[1] * a // 255, p[2] * a // 255)


def fit_stops_tol(px, tol):
    stops = [0]
    i = 0
    n = len(px)
    while i < n - 1:
        j = i + 1
        while j + 1 < n:
            ok = True
            for k in range(i + 1, j + 1):
                t = (k - i) / (j + 1 - i)
                for ch in range(4):
                    v = px[i][ch] + (px[j + 1][ch] - px[i][ch]) * t
                    if abs(v - px[k][ch]) > tol:
                        ok = False
                        break
                if not ok:
                    break
            if not ok:
                break
            j += 1
        stops.append(j)
        i = j
    return [(s_, px[s_]) for s_ in stops]


def fit_stops(px, cap=MAX_STOPS):
    """At most `cap` stops (index, colour) whose linear interpolation keeps
    closest to px (premultiplied ARGB tuples): the smallest tolerance that
    needs no more."""
    tol = 2
    while True:
        stops = fit_stops_tol(px, tol)
        if len(stops) <= cap:
            return stops
        tol += 2


def flt(c):
    s = f"{c:.5g}"
    if "." not in s and "e" not in s:
        s += ".0"
    return s + "f"


def argb(p):
    return f"0x{p[0]:02X}{p[1]:02X}{p[2]:02X}{p[3]:02X}u"


def corner_radius(pm, lines):
    """The outline's radius: where the diagonal from the corner reaches a
    mostly opaque pixel, k px in, an arc of radius about 2k does."""
    if lines[0] == 0 or lines[1] == 0:
        return 0
    k = 0
    while k < min(len(pm), len(pm[0])) // 2 and pm[k][k][0] < 128:
        k += 1
    return 2 * k


def shape(name, g, margins, lines):
    """The shape model of one state (see the module comment)."""
    h, w = len(g), len(g[0])
    pm = [[premul(p) for p in row] for row in g]
    nl = max(0, min(lines, (w - 1) // 2))
    nt = max(0, min(lines, (h - 1) // 2))
    n = [nl, nt, nl, nt]  # left, top, right, bottom
    edges = []
    for i in range(nl):  # left, then right: top to bottom
        edges.append((pm[nt][i], pm[h - 1 - nt][i]))
    for i in range(nt):  # top, then bottom: left to right
        edges.append((pm[i][nl], pm[i][w - 1 - nl]))
    for i in range(nl):
        edges.append((pm[nt][w - 1 - i], pm[h - 1 - nt][w - 1 - i]))
    for i in range(nt):
        edges.append((pm[h - 1 - i][nl], pm[h - 1 - i][w - 1 - nl]))
    # The face: its rows' averages down it and its columns' across it.
    xm, ym = w // 2, h // 2
    ys = range(nt, h - nt) if h - 2 * nt > 0 else range(ym, ym + 1)
    xs = range(nl, w - nl) if w - 2 * nl > 0 else range(xm, xm + 1)

    def mean(px):
        return tuple(round(sum(p[ch] for p in px) / len(px)) for ch in range(4))

    col = [mean([pm[y][x] for x in xs]) for y in ys]
    row = [mean([pm[y][x] for y in ys]) for x in xs]
    v = fit_stops(col)
    hz = fit_stops(row)

    def stops(st, length):
        return "{" + ", ".join(
            f"{{{round(i * 255 / max(length - 1, 1))}, {argb(c)}}}" for i, c in st) + "}"

    lines_c = []
    k = 0
    for side in range(4):
        cells = []
        for i in range(n[side]):
            a, b = edges[k]
            k += 1
            cells.append(f"{{{argb(a)}, {argb(b)}}}")
        lines_c.append("{" + ", ".join(cells) + "}" if cells else "{}")
    body = [f"    .w = {w}, .h = {h}, .radius = {corner_radius(pm, [nl, nt])},",
            f"    .lines = {{{nl}, {nt}, {nl}, {nt}}},",
            "    .edge = {" + ", ".join(lines_c) + "},",
            f"    .nv = {len(v)}, .v = {stops(v, len(col))},",
            f"    .nh = {len(hz)}, .hs = {stops(hz, len(row))},"]
    return [], body


def measure(style_dir, style_file, prefix, parts):
    st = Style(style_dir, style_file)
    out = []
    for cname, cls, part, band, states in parts:
        pr = st.props(cls, part)
        img = pr.get("imagefile") or pr.get("imagefile1")
        if "atlasrect" in pr:
            # Vista's and 7's DWM parts: a rectangle of one large image.
            img = pr["diskstream"]
            x0, y0, x1, y1 = (int(v) for v in pr["atlasrect"].split())
            g = [row[x0:x1] for row in load_png(f"{style_dir}/{img}")[y0:y1]]
        elif img is None:
            out.append(f"/* {prefix} {cls} {part}: no image. */")
            continue
        else:
            g = load_png(f"{style_dir}/{img}")
        count = int(pr.get("imagecount", "1"))
        horizontal = pr.get("imagelayout", "Vertical").lower() == "horizontal"
        m = [int(v) for v in pr.get("sizingmargins", "0 0 0 0").split()]
        sh = len(g) // count if not horizontal else len(g)
        sw = len(g[0]) // count if horizontal else len(g[0])
        idx = list(states) if states is not None else list(range(count))
        bodies = []
        for n in idx:
            if horizontal:
                sg = [row[n * sw:(n + 1) * sw] for row in g]
            else:
                sg = g[n * sh:(n + 1) * sh]
            decl, body = shape(f"{prefix}_{cname}_{n}", sg, m, band)
            out += decl
            bodies.append(body)
        out.append(f"/* {cls} {part} ({img}, states {idx[0] + 1} to {idx[-1] + 1}). */")
        out.append(f"static const xd_shape {prefix}_{cname}[{len(idx)}] = {{")
        for body in bodies:
            out.append("    {")
            out += ["    " + line for line in body]
            out.append("    },")
        out.append("};")
    return out


def rgb(p):
    return f"0x{p[0]:02X}{p[1]:02X}{p[2]:02X}u"


def far(p, q, tol=6):
    return max(abs(p[k] - q[k]) for k in range(3)) > tol or abs(p[3] - q[3]) > tol


def box_states(prefix, check, radio):
    """XP's 13 px check box and radio button (unchecked normal, hot,
    pressed, disabled, then checked): the border, the face's (or a hot
    ring's) gradient along the diagonal x + y, the colour inside a ring,
    and the mark's or dot's colours. Their shapes are drawn by luna.c."""
    out = []
    for name, g in (("check", check), ("radio", radio)):
        n = 13
        states = [g[k * n:(k + 1) * n] for k in range(8)]
        rows = []
        for k in range(8):
            s = states[k % 4]  # the unchecked state's face
            m = n - 2
            border = s[0][n // 2]
            # The diagonal gradient, read along the check box's outer ring
            # of face pixels (the radio's face is the check box's).
            cs = check[(k % 4) * n:(k % 4 + 1) * n]
            diag = []
            for d in range(2 * m - 1):
                x = 1 + min(d, m - 1)
                y = 1 + d - (x - 1)
                diag.append(cs[y][x])
            stops = fit_stops(diag)
            span = 2 * m - 2
            # Inside a hot ring: flat, unlike the face beneath it.
            inner_px = s[6][6] if name == "radio" else s[3][3]
            face_there = diag[10] if name == "radio" else diag[4]
            has_inner = far(inner_px, face_there)
            mark = [(0, 0, 0, 0)] * 3
            if k >= 4:
                c = states[k]
                if name == "check":
                    mark = [c[7][5]] * 3  # the bottom of the tick
                else:
                    mark = [c[5][5], c[6][6], c[7][7]]  # light, middle, dark
            rows.append(
                f"    {{.border = {rgb(border)}, .inner = {argb(premul(inner_px))}, "
                f".has_inner = {'true' if has_inner else 'false'},\n"
                f"     .mark = {{{', '.join(rgb(p) for p in mark)}}}, .count = {len(stops)},\n"
                f"     .stops = {{" + ", ".join(
                    f"{{{round(s_ * 255 / span)}, {argb(premul(p))}}}" for s_, p in stops)
                + "}},")
        out.append(f"/* {name} states: unchecked normal, hot, pressed, disabled; checked. */")
        out.append(f"static const xd_box_state {prefix}_{name}[8] = {{")
        out += rows
        out.append("};")
    return out


def glyph_colors(prefix, cname, g, count):
    """Each state's glyph colour: its first opaque pixel."""
    h = len(g) // count
    cols = []
    for k in range(count):
        c = next((p for row in g[k * h:(k + 1) * h] for p in row if p[3] == 255),
                 (0, 0, 0, 255))
        cols.append(rgb(c))
    return [f"static const uint32_t {prefix}_{cname}_glyph[{count}] = {{{', '.join(cols)}}};"]


def luna_extras(style_dir, style_file, prefix):
    st = Style(style_dir, style_file)
    out = []
    check = load_png(f"{style_dir}/{st.props('Button', 'CheckBox')['imagefile1']}")
    radio = load_png(f"{style_dir}/{st.props('Button', 'RadioButton')['imagefile1']}")
    out += box_states(prefix, check, radio)
    combo = st.props("ComboBox", "DropDownButton")
    out += glyph_colors(prefix, "dropdown",
                        load_png(f"{style_dir}/{combo['glyphimagefile']}"), 4)
    arrows = st.props("ScrollBar", "ArrowBtn")
    g = load_png(f"{style_dir}/{arrows['imagefile2']}")  # the 9 px glyphs
    out += glyph_colors(prefix, "scrollarrow", g, 16)
    # Caption button glyphs' colours: per glyph and state, the three
    # commonest (dark, middle, light: Silver's are embossed, the others
    # one colour).
    rows = []
    for part in ("CloseButton", "MaxButton", "MinButton", "RestoreButton", "HelpButton"):
        pr = st.props("Window", part)
        g = load_png(f"{style_dir}/{pr['imagefile3']}")  # the 13 px glyphs
        h = len(g) // 8
        states = []
        for k in range(8):
            count = {}
            for row in g[k * h:(k + 1) * h]:
                for p in row:
                    if p[3] == 255:
                        count[p[:3]] = count.get(p[:3], 0) + 1
            top = sorted(count, key=lambda c: -count[c])[:3]
            while len(top) < 3:
                top.append(top[-1])
            top.sort(key=lambda c: 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2])
            states.append("{" + ", ".join(rgb(c) for c in top) + "}")
        rows.append("    {" + ", ".join(states) + "},")
    out.append(f"/* Caption glyphs (close, maximize, minimize, restore, help), active"
               f" normal, hot, pressed, disabled, then inactive: dark, middle, light. */")
    out.append(f"static const uint32_t {prefix}_capglyph[5][8][3] = {{")
    out += rows
    out.append("};")
    # The thumb's gripper: light and dark lines, per state.
    g = load_png(f"{style_dir}/{st.props('ScrollBar', 'GripperVert')['imagefile']}")
    h = len(g) // 4
    out.append(f"static const uint32_t {prefix}_gripper[4][2] = {{"
               + ", ".join(f"{{{rgb(g[k * h][1])}, {rgb(g[k * h + 1][2])}}}" for k in range(4))
               + "};")
    # The tab page's gradient, top to bottom, stretched over the page.
    body = st.props("Tab", "Body")
    g = load_png(f"{style_dir}/{body.get('imagefile1') or body['imagefile']}")
    col = [premul(row[len(row) // 2]) for row in g]
    # Some are dithered: the gradient is their average over 15 rows.
    n = len(col)
    col = [tuple(round(sum(col[j][ch] for j in range(max(0, i - 7), min(n, i + 8)))
                       / (min(n, i + 8) - max(0, i - 7))) for ch in range(4))
           for i in range(n)]
    stops = fit_stops(col)
    out.append(f"static const xd_stop {prefix}_tabbody[] = {{"
               + ", ".join(f"{{{round(s * 255 / (len(col) - 1))}, {argb(p)}}}" for s, p in stops)
               + "};")
    return out


def luma(c):
    return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]


def commonest(pixels, n=1):
    """The n commonest opaque colours."""
    count = {}
    for p in pixels:
        if p[3] == 255:
            count[p[:3]] = count.get(p[:3], 0) + 1
    top = sorted(count, key=lambda c: -count[c])[:n]
    return top or [(0, 0, 0)]


def aero_extras(style_dir, style_file, prefix):
    st = Style(style_dir, style_file)
    out = []

    def strip(cls, part, key="imagefile"):
        pr = st.props(cls, part)
        if "atlasrect" in pr:
            x0, y0, x1, y1 = (int(v) for v in pr["atlasrect"].split())
            g = [row[x0:x1] for row in load_png(f"{style_dir}/{pr['diskstream']}")[y0:y1]]
        else:
            g = load_png(f"{style_dir}/{pr[key]}")
        count = int(pr.get("imagecount", "1"))
        h = len(g) // count
        return [g[k * h:(k + 1) * h] for k in range(count)]

    # Check boxes' ticks (checked: normal, hot, pressed, disabled): the
    # darkest pixel inside.
    boxes = strip("Button", "CheckBox", "imagefile1")
    ticks = []
    for k in range(4, 8):
        inside = [p for row in boxes[k][3:10] for p in row[3:10] if p[3] == 255]
        ticks.append(rgb(min(inside, key=luma)))
    out.append(f"static const uint32_t {prefix}_tick[4] = {{{', '.join(ticks)}}};")
    # Radio buttons, unchecked then checked: the border, the light ring
    # inside it, the face's gradient (top left, bottom right), and the
    # dot's rim, highlight, middle and dark.
    radios = strip("Button", "RadioButton", "imagefile1")
    rows = []
    for k in range(8):
        g = radios[k]
        u = radios[k % 4]
        vals = [g[1][5], u[2][5], u[4][3], u[8][8], g[8][4], g[5][5], g[6][6], g[8][6]]
        rows.append("    {" + ", ".join(rgb(p) for p in vals) + "},")
    out.append("/* Radio buttons: border, ring, face top left, face bottom right, dot rim,"
               " highlight, middle, dark. */")
    out.append(f"static const uint32_t {prefix}_radio[8][8] = {{")
    out += rows
    out.append("};")
    # Glyph colours: the drop-down arrow and the scroll arrows (per state).
    pr = st.props("ComboBox", "DropDownButtonRight")
    g = load_png(f"{style_dir}/{pr['imagefile1']}")
    h = len(g) // 4
    cols = [rgb(commonest([p for row in g[k * h:(k + 1) * h] for p in row])[0]) for k in range(4)]
    out.append(f"static const uint32_t {prefix}_dropdown_glyph[4] = {{{', '.join(cols)}}};")
    g = load_png(f"{style_dir}/{st.props('ScrollBar', 'ArrowBtn')['imagefile1']}")
    h = len(g) // 20
    cols = []
    for k in range(8):
        px = [p for row in g[k * h:(k + 1) * h] for p in row if p[3] == 255]
        cols.append(rgb(min(px, key=luma) if px else (0, 0, 0)))
    out.append(f"static const uint32_t {prefix}_scrollarrow_glyph[8] = {{{', '.join(cols)}}};")
    # The thumb's gripper: its dark and light lines.
    grip = strip("ScrollBar", "GripperVert", "imagefile1")[0]
    dark = min((p for row in grip for p in row if p[3] == 255), key=luma)
    light = max((p for row in grip for p in row if p[3] == 255), key=luma)
    out.append(f"static const uint32_t {prefix}_gripper[2] = {{{rgb(dark)}, {rgb(light)}}};")
    # Caption glyphs, glass (96 DPI, normal, hot, pressed, disabled) and
    # Basic (normal, hot, pressed, disabled; inactive the same): their fill
    # and outline.
    for name, parts in (("dwm", [("DWMWindow", f"{g}Glyph96", None) for g in
                                 ("Close", "Max", "Min", "Restore", "Help")]),
                        ("basic", [("Window", p, "imagefile1") for p in
                                   ("CloseButton", "MaxButton", "MinButton", "RestoreButton",
                                    "HelpButton")])):
        rows = []
        for cls, part, key in parts:
            states = strip(cls, part, key) if key is None else None
            if states is None:
                pr = st.props(cls, part)
                g = load_png(f"{style_dir}/{pr[key]}")
                h = len(g) // 8
                states = [g[k * h:(k + 1) * h] for k in range(8)]
            cells = []
            for k in range(len(states)):
                px = [p for row in states[k] for p in row]
                light = max(commonest(px, 3), key=luma)
                dark = min(commonest(px, 3), key=luma)
                cells.append(f"{{{rgb(light)}, {rgb(dark)}}}")
            rows.append("    {" + ", ".join(cells) + "},")
        n = 4 if name == "dwm" else 8
        out.append(f"/* {name} caption glyphs (close, maximize, minimize, restore, help):"
                   " fill and outline per state. */")
        out.append(f"static const uint32_t {prefix}_{name}_glyph[5][{n}][2] = {{")
        out += rows
        out.append("};")
    return out


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("luna", "aero"):
        sys.exit("usage: controls-measure.py luna|aero PKGS")
    which, pkgs = sys.argv[1], sys.argv[2]
    guard = f"XD_THEME_{which.upper()}_CONTROLS_H"
    src = ("luna.msstyles and royale.msstyles" if which == "luna" else "aero.msstyles")
    print(f"""/* SPDX-License-Identifier: MIT
 * Generated by tools/controls-measure.py from {src} (as imported by
 * xd-theme-import): the stretchable controls as measured numbers
 * (sizes, corner radii, colours and gradient stops; see xd_shape in
 * style.h), drawn procedurally by luna.c.
 * Do not edit by hand.
 */
#ifndef {guard}
#define {guard}

#include "style.h"
""")
    variants = LUNA_VARIANTS if which == "luna" else AERO_VARIANTS
    parts = LUNA_PARTS if which == "luna" else AERO_PARTS
    for prefix, pkg, style in variants:
        name = f"{which}_{prefix}" if which == "luna" else prefix
        print("\n".join(measure(f"{pkgs}/{pkg}", style, name, parts)))
        if which == "luna":
            print("\n".join(luna_extras(f"{pkgs}/{pkg}", style, name)))
        else:
            print("\n".join(aero_extras(f"{pkgs}/{pkg}", style, name)))
        print()
    print(f"#endif")


if __name__ == "__main__":
    main()
