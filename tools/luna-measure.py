#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measures the Luna (Windows XP) look from a 96 DPI screenshot and writes
the colour tables for the window frame, caption buttons and taskbar that
luna.c draws from:

    tools/luna-measure.py SCREENSHOT.png > src/theme/luna_data.h
    tools/luna-measure.py --variant silver SCREENSHOT.png > src/theme/luna_data_silver.h

Only colours are taken: per-row/per-column gradient profiles, edge lines,
corner masks, and smooth polynomial surfaces fitted to the caption buttons.
Glyphs, text and icons are drawn by the theme engine itself.

The coordinates below are where each element sits in the reference
screenshot (an active Notepad window, an inactive Explorer window and the
taskbar of a stock Windows XP SP3 desktop at 1920x972). Needs ImageMagick.
Silver (--variant silver) has its own layout and tables; see SILVER below.
"""
import re
import subprocess
import sys

VARIANT = "blue"
if len(sys.argv) == 4 and sys.argv[1] == "--variant":
    VARIANT = sys.argv[2]
elif len(sys.argv) != 2:
    sys.exit("usage: luna-measure.py [--variant blue|silver] SCREENSHOT.png")
if VARIANT not in ("blue", "silver"):
    sys.exit(f"unknown variant {VARIANT!r}")
REF = sys.argv[-1]

# Reference layout.
ACTIVE = dict(x0=88, y0=116, x1=640, y1=438, mid=400,
              buttons=[(568, 122), (591, 122), (614, 122)])
INACTIVE = dict(x0=717, y0=192, x1=1516, y1=791, mid=1000,
                buttons=[(1444, 198), (1467, 198), (1490, 198)])
BAR_Y = 942            # taskbar top row
START = dict(left=(0, 8), right=(76, 99))   # [from, to) columns
TASK = {               # normal and pressed task buttons
    0: dict(left=(106, 110), mid=240, right=(261, 266)),
    1: dict(left=(266, 271), mid=400, right=(421, 426)),
}
TASK_ROWS = (3, 28)    # rows within the bar
TRAY = dict(left=(1755, 1760), a=1760, b=1919)
CAPTION_H = 30
BORDER = 4
# Top corners: transparent pixels per column, from the outer edge in.
CORNER_CUT = [5, 3, 2, 1, 1]


def region(x, y, w, h):
    out = subprocess.run(["magick", REF, "-crop", f"{w}x{h}+{x}+{y}", "+repage", "txt:-"],
                         capture_output=True, text=True, check=True).stdout.splitlines()[1:]
    g = [[(0, 0, 0)] * w for _ in range(h)]
    for line in out:
        m = re.match(r"(\d+),(\d+): \(([\d.]+),([\d.]+),([\d.]+)", line)
        if m:
            g[int(m.group(2))][int(m.group(1))] = tuple(int(float(m.group(i))) for i in (3, 4, 5))
    return g


def hexc(p, alpha=0xFF):
    return f"0x{alpha:02X}{p[0]:02X}{p[1]:02X}{p[2]:02X}"


def column(x, y, h):
    return [r[0] for r in region(x, y, 1, h)]


def table(name, rows, cols_per_line=8):
    """rows: list of lists of strings."""
    out = [f"static const uint32_t {name} = {{"]
    for r in rows:
        if isinstance(r, list):
            out.append("    {" + ", ".join(r) + "},")
        else:
            out.append("    " + r + ",")
    out.append("};")
    return "\n".join(out)


def poly_fit(g, mask):
    """Degree-4 surface per channel over (u, v) in [0, 1]^2."""
    h, w = len(g), len(g[0])
    terms = [(a, b) for a in range(5) for b in range(5 - a)]
    pts = [(i, j) for j in range(h) for i in range(w) if mask(i, j)]
    coef = []
    for ch in range(3):
        n = len(terms)
        A = [[0.0] * (n + 1) for _ in range(n)]
        for (i, j) in pts:
            u, v = i / (w - 1), j / (h - 1)
            f = [u ** a * v ** b for (a, b) in terms]
            for p in range(n):
                A[p][n] += f[p] * g[j][i][ch]
                for q in range(n):
                    A[p][q] += f[p] * f[q]
        for c in range(n):  # Gauss-Jordan
            piv = max(range(c, n), key=lambda r: abs(A[r][c]))
            A[c], A[piv] = A[piv], A[c]
            for r in range(n):
                if r != c:
                    fct = A[r][c] / A[c][c]
                    A[r] = [A[r][k] - fct * A[c][k] for k in range(n + 1)]
        coef.append([A[i][n] / A[i][i] for i in range(n)])
    return terms, coef


def frame_tables(tag, f):
    x0, y0, x1, y1 = f["x0"], f["y0"], f["x1"], f["y1"]
    cap = region(x0, y0, x1 - x0 + 1, CAPTION_H)
    w = x1 - x0 + 1
    mid = [hexc(cap[r][f["mid"] - x0]) for r in range(CAPTION_H)]
    left, right = [], []
    for r in range(CAPTION_H):
        lrow, rrow = [], []
        for c in range(8):
            cut = c < len(CORNER_CUT) and r < CORNER_CUT[c]
            lrow.append(hexc(cap[r][c], 0 if cut else 0xFF))
        for c in range(5):          # c = 0 is 4 from the outer edge
            k = 4 - c               # distance from the outer edge
            cut = k < len(CORNER_CUT) and r < CORNER_CUT[k]
            rrow.append(hexc(cap[r][w - 5 + c], 0 if cut else 0xFF))
        left.append(lrow)
        right.append(rrow)
    ymid = (y0 + y1) // 2
    side_l = [hexc(p) for p in region(x0, ymid, BORDER, 1)[0]]
    side_r = [hexc(p) for p in region(x1 - BORDER + 1, ymid, BORDER, 1)[0]]
    xmid = (x0 + x1) // 2
    bottom = [hexc(p) for p in column(xmid, y1 - BORDER + 1, BORDER)]
    bl = [[hexc(p) for p in row] for row in region(x0, y1 - BORDER + 1, BORDER, BORDER)]
    br = [[hexc(p) for p in row] for row in region(x1 - BORDER + 1, y1 - BORDER + 1, BORDER, BORDER)]
    out = [
        f"/* {tag} caption: middle column, top to bottom. */",
        table(f"luna_{tag}_cap_mid[{CAPTION_H}]", mid),
        f"/* {tag} caption: the 8 left and 5 right columns (alpha 0 = cut corner). */",
        table(f"luna_{tag}_cap_left[{CAPTION_H}][8]", left),
        table(f"luna_{tag}_cap_right[{CAPTION_H}][5]", right),
        f"/* {tag} borders: left and right outside to in... left: outer first;"
        " right: inner first; bottom: top row first. */",
        table(f"luna_{tag}_side_left[{BORDER}]", side_l),
        table(f"luna_{tag}_side_right[{BORDER}]", side_r),
        table(f"luna_{tag}_bottom[{BORDER}]", bottom),
        table(f"luna_{tag}_corner_bl[{BORDER}][{BORDER}]", bl),
        table(f"luna_{tag}_corner_br[{BORDER}][{BORDER}]", br),
    ]
    # Caption buttons: face surfaces (19x19 inside the border).
    names = ["minimize", "maximize", "close"]
    for (bx, by), name in zip(f["buttons"], names):
        g = region(bx + 1, by + 1, 19, 19)
        def mask(i, j, g=g):
            p = g[j][i]
            return not (p[0] > 200 and p[1] > 200 and p[2] > 200)
        terms, coef = poly_fit(g, mask)
        out.append(f"/* {tag} {name} button face: c = sum k * u^a * v^b, u, v in [0, 1]. */")
        out.append(f"static const float luna_{tag}_{name}_face[3][{len(terms)}] = {{")
        for ch in coef:
            out.append("    {" + ", ".join(f"{c:.3f}f" for c in ch) + "},")
        out.append("};")
        ring = region(bx, by, 21, 21)
        out.append(f"static const uint32_t luna_{tag}_{name}_corner[3][3] = {{")
        for r in range(3):
            out.append("    {" + ", ".join(hexc(ring[r][c]) for c in range(3)) + "},")
        out.append("};")
    border = region(f["buttons"][0][0] + 10, f["buttons"][0][1], 1, 1)[0][0]
    out.append(f"static const uint32_t luna_{tag}_button_border = {hexc(border)};")
    return "\n".join(out)


def bar_tables():
    out = []
    bar = column(1000, BAR_Y, 30)
    out.append("/* Taskbar, top to bottom. */")
    out.append(table("luna_taskbar[30]", [hexc(p) for p in bar]))
    g = region(START["left"][0], BAR_Y, START["left"][1] - START["left"][0], 30)
    out.append("/* Start button: left columns, then right columns (the text and logo"
               " area between is interpolated). */")
    out.append(table(f"luna_start_left[30][{len(g[0])}]", [[hexc(p) for p in r] for r in g]))
    g = region(START["right"][0], BAR_Y, START["right"][1] - START["right"][0], 30)
    out.append(table(f"luna_start_right[30][{len(g[0])}]", [[hexc(p) for p in r] for r in g]))
    r0, r1 = TASK_ROWS
    for state, t in TASK.items():
        nm = ["normal", "pressed"][state]
        g = region(t["left"][0], BAR_Y + r0, t["left"][1] - t["left"][0], r1 - r0)
        out.append(f"/* Task button ({nm}): left, middle and right columns. */")
        out.append(table(f"luna_task_{nm}_left[{r1 - r0}][{len(g[0])}]",
                         [[hexc(p) for p in r] for r in g]))
        out.append(table(f"luna_task_{nm}_mid[{r1 - r0}]",
                         [hexc(p) for p in column(t["mid"], BAR_Y + r0, r1 - r0)]))
        g = region(t["right"][0], BAR_Y + r0, t["right"][1] - t["right"][0], r1 - r0)
        out.append(table(f"luna_task_{nm}_right[{r1 - r0}][{len(g[0])}]",
                         [[hexc(p) for p in r] for r in g]))
    g = region(TRAY["left"][0], BAR_Y, TRAY["left"][1] - TRAY["left"][0], 30)
    out.append("/* Notification area: left edge, then the body at its left and right"
               " ends (interpolated between). */")
    out.append(table(f"luna_tray_left[30][{len(g[0])}]", [[hexc(p) for p in r] for r in g]))
    out.append(table("luna_tray_a[30]", [hexc(p) for p in column(TRAY["a"], BAR_Y, 30)]))
    out.append(table("luna_tray_b[30]", [hexc(p) for p in column(TRAY["b"], BAR_Y, 30)]))
    return "\n".join(out)


def main_blue():
    print("""/* SPDX-License-Identifier: MIT
 * Generated by tools/luna-measure.py from a Windows XP screenshot: colour
 * profiles of the Luna frame, caption buttons and taskbar. Colours only
 * (0xAARRGGBB); the shapes, glyphs and text are drawn by style_modern.c.
 * Do not edit by hand.
 */
#ifndef XD_THEME_LUNA_DATA_H
#define XD_THEME_LUNA_DATA_H

#include <stdint.h>
""")
    print(frame_tables("active", ACTIVE))
    print()
    print(frame_tables("inactive", INACTIVE))
    print()
    print(bar_tables())
    print("\n#endif")


# ---- Silver ---------------------------------------------------------------------
#
# XP's Silver ("Metallic") has its own artwork, lighter and glossier than a
# recoloured Blue, so it is measured too. The reference is toastytech.com's
# Windows XP Silver screenshot, http://toastytech.com/guis/wxpsilver.png
# (800x600, 96 DPI; not redistributed here, pass its path): Display
# Properties, active, open on the Appearance tab with Silver selected. Its
# preview holds an inactive window and an active one; below are two task
# buttons and the notification area.
#
# The windows there have XP's fixed (dialog) frame, 3px, where luna.c draws
# the 4px sizing frame Blue was measured with. Each 3-line edge becomes 4
# by doubling its light middle line, and the caption gains a row where its
# glossy top meets the gradient (row 5, the first below the rounded
# corner), which puts the caption buttons 6 rows down as on Blue.
#
# The active window covers part of the inactive one: the inactive
# caption's last 7 rows are taken at its left end (x 206, left of the
# active window), its right end there mirrors its left, and the hidden
# bottom of its caption buttons follows the active buttons' bottom rows,
# mapped colour for colour by a per-channel line fitted on the rows both
# show. The reference has no inactive Help button (its face is the
# inactive Minimize face, its glyph the active one's in the inactive
# glyph colours), no Restore glyph, no pressed task button and no hot or
# pressed states; luna.c derives those.
SILVER = dict(
    active=dict(
        x0=161, y0=34, x1=564, y1=488,     # Display Properties
        mid=[(400, 0, 29)],               # caption columns: (x, from row, to row)
        right_rows=29,                    # caption rows visible at the right end
        side_y=300, bottom_x=300,
        side_right=True, corner_br=True,
        # The preview's active window: Minimize, Maximize and Close; Display
        # Properties: Help.
        buttons=dict(minimize=(457, 140), maximize=(480, 140), close=(503, 140),
                     help=(516, 39)),
        button_rows=21,
    ),
    inactive=dict(
        x0=198, y0=113, x1=511, y1=249,    # the preview's inactive window
        mid=[(350, 0, 22), (206, 22, 29)],
        right_rows=22,
        side_y=180, bottom_x=202,
        side_right=False, corner_br=False,  # hidden: mirrored from the left
        buttons=dict(minimize=(440, 118), maximize=(463, 118), close=(486, 118)),
        button_rows=17,
    ),
)
SILVER_CAPTION_ROWS = 29   # 3px frame + caption, as the reference shows it
SILVER_EXTRA_ROW = 5       # doubled to make the 30 rows luna.c draws
SILVER_DESKTOP = (0x58, 0x57, 0x68)
SILVER_BAR_Y = 570
SILVER_BAR_X = 650         # an empty stretch of taskbar
SILVER_START = dict(left=(0, 8), right=(76, 99))
SILVER_TASK = dict(left=(105, 109), mid=200, right=(259, 264))
SILVER_TRAY = dict(left=(707, 712), a=712, b=799)
# Cancel (a normal push button) and Apply (disabled), 75x23 with the ring.
SILVER_PUSH = dict(left=(400, 405), mid=410, right=(470, 475), y=456)
SILVER_PUSH_DISABLED = dict(border=(482, 467), face=(490, 467))
# Caption button glyphs lie within these rows and columns of the 21x21 button.
GLYPH_SPAN = (4, 16)

_PIX = None


def pix(x, y):
    """The reference's pixel (r, g, b), read once as raw RGB."""
    global _PIX
    if _PIX is None:
        size = subprocess.run(["magick", "identify", "-format", "%w %h", REF],
                              capture_output=True, text=True, check=True).stdout.split()
        raw = subprocess.run(["magick", REF, "-depth", "8", "rgb:-"],
                             capture_output=True, check=True).stdout
        _PIX = (int(size[0]), raw)
    w, raw = _PIX
    i = (y * w + x) * 3
    return (raw[i], raw[i + 1], raw[i + 2])


def luma(p):
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]


def expand3(v):
    """A 3-line edge (outer, middle, inner, in either order) as 4 lines."""
    assert len(v) == 3
    return [v[0], v[1], v[1], v[2]]


def silver_caption(f):
    """The caption's middle column and its 8 left and 5 right columns, as
    the reference shows them: (r, g, b), or None where the corner is cut."""
    x0, y0, x1 = f["x0"], f["y0"], f["x1"]
    mid = [None] * SILVER_CAPTION_ROWS
    for (x, a, b) in f["mid"]:
        for r in range(a, b):
            mid[r] = pix(x, y0 + r)
    left, right = [], []
    for r in range(SILVER_CAPTION_ROWS):
        lrow = []
        for c in range(8):
            cut = c < len(CORNER_CUT) and r < CORNER_CUT[c]
            p = pix(x0 + c, y0 + r)
            assert not cut or p == SILVER_DESKTOP, ("corner", r, c, p)
            lrow.append(None if cut else p)
        rrow = []
        for c in range(5):          # c = 0 is 4 from the outer edge
            k = 4 - c
            cut = k < len(CORNER_CUT) and r < CORNER_CUT[k]
            p = pix(x1 - 4 + c, y0 + r) if r < f["right_rows"] else lrow[k]
            rrow.append(None if cut else p)
        left.append(lrow)
        right.append(rrow)
    return mid, left, right


def with_extra_row(rows):
    return rows[:SILVER_EXTRA_ROW] + [rows[SILVER_EXTRA_ROW]] + rows[SILVER_EXTRA_ROW:]


def hexo(p):
    return "0x00000000" if p is None else hexc(p)


def fit_line(pairs):
    """Least-squares b = k a + m over (a, b) pairs."""
    n = len(pairs)
    sa = sum(a for a, _ in pairs)
    sb = sum(b for _, b in pairs)
    saa = sum(a * a for a, _ in pairs)
    sab = sum(a * b for a, b in pairs)
    d = n * saa - sa * sa
    k = (n * sab - sa * sb) / d if d else 1.0
    return k, (sb - k * sa) / n


def measure_button(x, y, rows, close):
    """A caption button's face with its glyph painted out (rows from `rows`
    on are None: hidden) and the glyph, {(i, j): (r, g, b)}. Glyph pixels
    are those far from their row's face colour, column 17 (clear of every
    glyph) on the flat grey buttons. Close's face shades across the row,
    so there the glyph is its white X and the outline darker than the
    row's median (the thin X never fills a row)."""
    face = [[pix(x + i, y + j) if j < rows else None for i in range(21)] for j in range(21)]
    glyph = {}
    lo, hi = GLYPH_SPAN
    for j in range(lo, min(hi + 1, rows)):
        row = sorted((face[j][i] for i in range(1, 20)), key=luma)
        med = row[len(row) // 2] if close else face[j][17]
        for i in range(lo, hi + 1):
            p = face[j][i]
            far = max(abs(p[c] - med[c]) for c in range(3))
            if close:
                on = min(p) >= 0xF0 or (far > 40 and luma(p) < luma(med))
            else:
                on = far > 20
            if on:
                glyph[(i, j)] = p
        # Paint the glyph out along the row, between the face pixels.
        for i in range(lo, hi + 1):
            if (i, j) not in glyph:
                continue
            a = i - 1
            while (a, j) in glyph:
                a -= 1
            b = i + 1
            while (b, j) in glyph:
                b += 1
            t = (i - a) / (b - a)
            pa, pb = face[j][a], face[j][b]
            face[j][i] = tuple(round(pa[c] + (pb[c] - pa[c]) * t) for c in range(3))
    return face, glyph


def fill_hidden_rows(face, rows, active_face):
    """Rows the reference hides: the active button's, mapped per channel by
    a line fitted on the rows both show."""
    fits = []
    for c in range(3):
        pairs = [(active_face[j][i][c], face[j][i][c])
                 for j in range(1, rows) for i in range(1, 20)]
        fits.append(fit_line(pairs))
    for j in range(rows, 21):
        face[j] = [tuple(max(0, min(255, round(fits[c][0] * p[c] + fits[c][1])))
                         for c in range(3)) for p in active_face[j]]


def glyph_tables(name, glyph):
    inks = []
    for j in range(21):
        for i in range(21):
            p = glyph.get((i, j))
            if p is not None and p not in inks:
                inks.append(p)
    out = [f"static const char *const {name}_glyph[21] = {{"]
    for j in range(21):
        cells = "".join("." if (i, j) not in glyph else chr(ord("a") + inks.index(glyph[(i, j)]))
                        for i in range(21))
        out.append(f'    "{cells}",')
    out.append("};")
    out.append(table(f"{name}_ink[{len(inks)}]", [hexc(p) for p in inks]))
    return out


def face_table(name, face):
    rows = [[hexo(None if i in (0, 20) and j in (0, 20) else face[j][i]) for i in range(21)]
            for j in range(21)]
    return table(f"{name}_face[21][21]", rows)


def silver_frame_tables(tag, f, active_faces=None, active_glyphs=None):
    mid, left, right = silver_caption(f)
    assert all(p is not None for p in mid)
    mid, left, right = with_extra_row(mid), with_extra_row(left), with_extra_row(right)
    x0, x1, y1 = f["x0"], f["x1"], f["y1"]
    side_l = expand3([pix(x0 + c, f["side_y"]) for c in range(3)])
    side_r = expand3([pix(x1 - 2 + c, f["side_y"]) for c in range(3)]) if f["side_right"] \
        else list(reversed(side_l))
    bottom = expand3([pix(f["bottom_x"], y1 - 2 + r) for r in range(3)])
    bl = expand3([expand3([pix(x0 + c, y1 - 2 + r) for c in range(3)]) for r in range(3)])
    br = expand3([expand3([pix(x1 - 2 + c, y1 - 2 + r) for c in range(3)]) for r in range(3)]) \
        if f["corner_br"] else [list(reversed(r)) for r in bl]
    n = f"luna_silver_{tag}"
    out = [
        f"/* {tag} caption: middle column, top to bottom. */",
        table(f"{n}_cap_mid[{CAPTION_H}]", [hexc(p) for p in mid]),
        f"/* {tag} caption: the 8 left and 5 right columns (alpha 0 = cut corner). */",
        table(f"{n}_cap_left[{CAPTION_H}][8]", [[hexo(p) for p in r] for r in left]),
        table(f"{n}_cap_right[{CAPTION_H}][5]", [[hexo(p) for p in r] for r in right]),
        f"/* {tag} borders: left: outer first; right: inner first; bottom: top row first. */",
        table(f"{n}_side_left[{BORDER}]", [hexc(p) for p in side_l]),
        table(f"{n}_side_right[{BORDER}]", [hexc(p) for p in side_r]),
        table(f"{n}_bottom[{BORDER}]", [hexc(p) for p in bottom]),
        table(f"{n}_corner_bl[{BORDER}][{BORDER}]", [[hexc(p) for p in r] for r in bl]),
        table(f"{n}_corner_br[{BORDER}][{BORDER}]", [[hexc(p) for p in r] for r in br]),
    ]
    faces, glyphs = {}, {}
    for name in ("minimize", "maximize", "close", "help"):
        if name in f["buttons"]:
            bx, by = f["buttons"][name]
            face, glyph = measure_button(bx, by, f["button_rows"], name == "close")
            if f["button_rows"] < 21:
                fill_hidden_rows(face, f["button_rows"], active_faces[name])
        else:
            # Inactive Help: the Minimize face; the active glyph in the
            # inactive glyph colours, matched pixel for pixel on Minimize
            # and Maximize.
            face = [list(r) for r in faces["minimize"]]
            ink = {}
            for other in ("minimize", "maximize"):
                for pos, p in active_glyphs[other].items():
                    if pos in glyphs[other]:
                        ink.setdefault(p, glyphs[other][pos])
            glyph = {pos: ink[p] for pos, p in active_glyphs[name].items()}
        faces[name], glyphs[name] = face, glyph
        out.append(f"/* {tag} {name} button: the face (its glyph painted out; alpha 0 = cut"
                   " corner), then the glyph ('.': none; letters index the ink). */")
        out.append(face_table(f"{n}_{name}", face))
        out += glyph_tables(f"{n}_{name}", glyph)
    # For glyphs the reference doesn't show (Restore): Maximize's outline,
    # title bar fill and light edge.
    g = glyphs["maximize"]
    out.append(f"/* {tag} glyph colours: outline, fill, light edge. */")
    out.append(table(f"{n}_glyph_colors[3]", [hexc(g[(5, 5)]), hexc(g[(6, 6)]), hexc(g[(4, 6)])]))
    return "\n".join(out), faces, glyphs


def silver_bar_tables():
    y0 = SILVER_BAR_Y

    def block(name, xs, rows, top):
        a, b = xs
        return table(f"{name}[{rows}][{b - a}]",
                     [[hexc(pix(x, top + r)) for x in range(a, b)] for r in range(rows)])

    def col(name, x, rows, top):
        return table(f"{name}[{rows}]", [hexc(pix(x, top + r)) for r in range(rows)])

    out = ["/* Taskbar, top to bottom. */", col("luna_silver_taskbar", SILVER_BAR_X, 30, y0)]
    out.append("/* Start button: left columns, then right columns (the text and logo"
               " area between is interpolated). */")
    out.append(block("luna_silver_start_left", SILVER_START["left"], 30, y0))
    out.append(block("luna_silver_start_right", SILVER_START["right"], 30, y0))
    r0, r1 = TASK_ROWS
    t = SILVER_TASK
    out.append("/* Task button (normal): left, middle and right columns. */")
    out.append(block("luna_silver_task_normal_left", t["left"], r1 - r0, y0 + r0))
    out.append(col("luna_silver_task_normal_mid", t["mid"], r1 - r0, y0 + r0))
    out.append(block("luna_silver_task_normal_right", t["right"], r1 - r0, y0 + r0))
    tr = SILVER_TRAY
    out.append("/* Notification area: left edge, then the body at its left and right"
               " ends (interpolated between). */")
    out.append(block("luna_silver_tray_left", tr["left"], 30, y0))
    out.append(col("luna_silver_tray_a", tr["a"], 30, y0))
    out.append(col("luna_silver_tray_b", tr["b"], 30, y0))
    p = SILVER_PUSH
    out.append("/* Push button (Cancel), 75x23 with its ring: the five columns at each"
               " end and the middle column, top to bottom. */")
    for name, (a, b) in (("left", p["left"]), ("right", p["right"])):
        out.append(table(f"luna_silver_button_{name}[5][23]",
                         [[hexc(pix(x, p["y"] + r)) for r in range(23)] for x in range(a, b)]))
    out.append(col("luna_silver_button_mid", p["mid"], 23, p["y"]))
    d = SILVER_PUSH_DISABLED
    out.append("/* A disabled push button (Apply): border and face. */")
    out.append("static const uint32_t luna_silver_button_disabled_border = "
               f"{hexc(pix(*d['border']))};")
    out.append(f"static const uint32_t luna_silver_button_disabled_face = {hexc(pix(*d['face']))};")
    return "\n".join(out)


def main_silver():
    print("""/* SPDX-License-Identifier: MIT
 * Generated by tools/luna-measure.py --variant silver from a Windows XP
 * Silver screenshot: colour profiles of the Silver frame, caption buttons,
 * taskbar and push button, as luna_data.h has Blue's. Colours only
 * (0xAARRGGBB); shapes and text are drawn by luna.c. Do not edit by hand.
 */
#ifndef XD_THEME_LUNA_DATA_SILVER_H
#define XD_THEME_LUNA_DATA_SILVER_H

#include <stdint.h>
""")
    text, faces, glyphs = silver_frame_tables("active", SILVER["active"])
    print(text)
    print()
    text, _, _ = silver_frame_tables("inactive", SILVER["inactive"], faces, glyphs)
    print(text)
    print()
    print(silver_bar_tables())
    print("\n#endif")


if VARIANT == "blue":
    main_blue()
else:
    main_silver()
