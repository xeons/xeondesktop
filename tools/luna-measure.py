#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measures the Luna (Windows XP) look from a 96 DPI screenshot and writes
the colour tables for the window frame, caption buttons and taskbar that
luna.c draws from:

    tools/luna-measure.py SCREENSHOT.png > src/theme/luna_data.h
    tools/luna-measure.py --variant silver SCREENSHOT.png > src/theme/luna_data_silver.h
    tools/luna-measure.py --variant olive SCREENSHOT.png > src/theme/luna_data_olive.h
    tools/luna-measure.py --variant energyblue SCREENSHOT.png STARTMENU.png \
        > src/theme/luna_data_energyblue.h

Only colours are taken: per-row/per-column gradient profiles, edge lines,
corner masks, and smooth polynomial surfaces fitted to the caption buttons.
Glyphs, text and icons are drawn by the theme engine itself.

The coordinates below are where each element sits in the reference
screenshot (an active Notepad window, an inactive Explorer window and the
taskbar of a stock Windows XP SP3 desktop at 1920x972). Needs ImageMagick.
Silver, Olive Green and Energy Blue (--variant) have their own references,
layouts and tables; see LAYOUTS below.
"""
import re
import subprocess
import sys

VARIANT = "blue"
START_REF = None
args = sys.argv[1:]
if len(args) >= 3 and args[0] == "--variant":
    VARIANT = args[1]
    args = args[2:]
if VARIANT not in ("blue", "silver", "olive", "energyblue"):
    sys.exit(f"unknown variant {VARIANT!r}")
if len(args) != (2 if VARIANT == "energyblue" else 1):
    sys.exit("usage: luna-measure.py [--variant silver|olive] SCREENSHOT.png\n"
             "       luna-measure.py --variant energyblue SCREENSHOT.png STARTMENU.png")
REF = args[0]
if VARIANT == "energyblue":
    START_REF = args[1]

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


def drop_window_icon(rows):
    """The reference window's own icon starts at the caption's 7th column,
    inside the left end measured here. Where columns 7 and 8 differ sharply
    from the caption beside them (column 6), below the rounded corner, they
    are the icon's pixels, not the frame's: take the caption's colour. Real
    shading there changes by up to 12 levels; an icon's edge by more."""
    def rgb(h):
        v = int(h, 16) & 0xFFFFFF
        return ((v >> 16) & 255, (v >> 8) & 255, v & 255)
    for y, row in enumerate(rows):
        if y < 5:
            continue
        for c in (6, 7):
            if max(abs(a - b) for a, b in zip(rgb(row[c]), rgb(row[5]))) > 24:
                row[c] = row[5]
    return rows


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
        table(f"luna_{tag}_cap_left[{CAPTION_H}][8]", drop_window_icon(left)),
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


# ---- Silver, Olive Green -----------------------------------------------------------
#
# The other variants have their own artwork (Silver's is lighter and
# glossier than a recoloured Blue would be, Olive Green's shading differs
# too), so they are measured from screenshots of their own, each with a
# layout below. These tables are whole: the caption buttons are taken
# pixel for pixel (their glyph separated from the face, so luna.c can light
# the face when hot), and nothing is fitted.
#
# Silver: toastytech.com's Windows XP Silver screenshot,
# http://toastytech.com/guis/wxpsilver.png (800x600, 96 DPI; not
# redistributed here, pass its path): Display Properties, active, open on
# the Appearance tab with Silver selected. Its preview holds an inactive
# window and an active one; below are two task buttons and the
# notification area.
#
# Its windows have XP's fixed (dialog) frame, 3px, where luna.c draws the
# 4px sizing frame Blue was measured with. Each 3-line edge becomes 4 by
# doubling its light middle line, and the caption gains a row where its
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
#
# Energy Blue (the "Royale" theme Microsoft shipped for Media Center and
# Tablet PC editions): Jason Prahl's screenshot,
# https://www.jasonprahl.com/wp-content/uploads/2006/01/xp.png (1364x839,
# full colour; not redistributed here), an XP desktop in a virtual machine
# whose frame takes the left 10 columns and the bottom rows: the guest's
# x 0 is the image's x 10. It is 1:1 (the caption is 30 rows, the caption
# buttons 21px, as XP's metrics give). It shows an active Explorer window
# and the taskbar with one, pressed, task button; it has no inactive
# window, no normal task button and no Help button: luna.c fades the
# active artwork for an inactive window (as for Olive Green), recolours
# Blue's normal task button, and draws Help on the Minimize face. Energy
# Blue's caption buttons sit 5 columns from the frame's right edge where
# Blue's sit 6; luna.c keeps Blue's layout.
#
# Its Start menu comes from a second screenshot, from the Reddit post "A
# retrospective on the Luna theme and Royale" (1024x768, a
# cf.preview.redd.it image; not redistributed here). That image is
# quantized to a 256-colour palette, so its colours are averages (or the
# commonest colour) over regions, not single pixels.
#
# Olive Green: a screenshot hosted on DeviantArt,
# https://images-wixmp-ed30a86b8c4ca887773594c2.wixmp.com/i/a844a3cf-6fdd-4ee5-85e5-f51c27600fd7/dzg2-bb82fa90-d395-4600-b555-aa0274bd3ad1.png
# (800x600, lossless; not redistributed here). It is a pre-release build
# ("For testing purposes only. Build 2485.main.010531-2130", May 2001), so
# final XP's Olive Green may differ in detail. It shows an active Internet
# Explorer window (a 4px sizing frame and 30-row caption, as Blue's) and
# the taskbar with normal task buttons and a pressed one. It has no
# inactive window and no Help button; luna.c derives those (and the hot
# and pressed states) from the active artwork.
LAYOUTS = dict(
    silver=dict(
        title="Silver",
        desktop=(0x58, 0x57, 0x68),  # behind the cut corners
        caption_rows=29,             # 3px frame + caption, as the reference shows it
        extra_row=5,                 # doubled to make the 30 rows luna.c draws
        edge3=True,                  # 3-line edges, doubled to 4
        white_glyphs=False,          # dark, embossed glyphs
        active=dict(
            x0=161, y0=34, x1=564, y1=488,     # Display Properties
            mid=[(400, 0, 29)],               # caption columns: (x, from row, to row)
            right_rows=29,                    # caption rows visible at the right end
            side_y=300, bottom_x=300,
            side_right=True, corner_br=True,
            # The preview's active window: Minimize, Maximize and Close;
            # Display Properties: Help.
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
        bar=dict(y=570, x=650,                 # x: an empty stretch of taskbar
                 start=dict(left=(0, 8), right=(76, 99)),
                 task=dict(left=(105, 109), mid=200, right=(259, 264)),
                 tray=dict(left=(707, 712), a=712, b=799)),
        # Cancel (a normal push button) and Apply (disabled), 75x23 with the ring.
        push=dict(left=(400, 405), mid=410, right=(470, 475), y=456,
                  disabled_border=(482, 467), disabled_face=(490, 467)),
    ),
    olive=dict(
        title="Olive Green",
        desktop=None,                # a wallpaper
        caption_rows=30,
        extra_row=None,
        edge3=False,
        white_glyphs=True,
        active=dict(
            x0=5, y0=184, x1=264, y1=563,      # Internet Explorer
            mid=[(190, 0, 30)],               # between the title and the buttons
            right_rows=30,
            side_y=400, bottom_x=100,
            side_right=True, corner_br=True,
            buttons=dict(minimize=(192, 190), maximize=(215, 190), close=(238, 190)),
            button_rows=21,
        ),
        inactive=None,
        bar=dict(y=570, x=620,
                 start=dict(left=(0, 8), right=(76, 99)),
                 task=dict(left=(436, 440), mid=560, right=(591, 596)),  # "untitled - Paint"
                 task_pressed=dict(left=(276, 281), mid=425, right=(431, 436)),
                 tray=dict(left=(635, 640), a=650, b=799)),
        push=None,
    ),
    energyblue=dict(
        title="Energy Blue",
        desktop=None,                # a wallpaper
        caption_rows=30,
        extra_row=None,
        edge3=False,
        white_glyphs=True,
        active=dict(
            x0=34, y0=189, x1=831, y1=786,     # My Documents
            mid=[(500, 0, 30)],
            right_rows=30,
            side_y=500, bottom_x=500,
            side_right=True, corner_br=True,
            buttons=dict(minimize=(760, 195), maximize=(783, 195), close=(806, 195)),
            button_rows=21,
        ),
        inactive=None,
        bar=dict(y=802, x=700,
                 start=dict(left=(10, 18), right=(86, 109)),
                 task=None,
                 task_pressed=dict(left=(169, 174), mid=300, right=(322, 327)),  # My Documents
                 tray=dict(left=(1228, 1233), a=1233, b=1356)),
        push=None,
        # The Start menu screenshot: (x, y, w, h) regions.
        startmenu=dict(
            frame=(200, 260, 170, 1),        # the top edge
            head_a=(200, 261, 170, 3),       # right of the user's name
            head_b=(200, 320, 170, 3),
            left_bg=(5, 330, 175, 360),      # the programs: commonest colour
            left_sub=(45, 352, 60, 10),      # "Internet Explorer", under "Internet"
            left_sep=(20, 417, 140, 1),
            column_edge=(188, 330, 1, 360),
            right_bg=(195, 330, 180, 360),
            right_sep=(200, 487, 170, 1),
            foot_a=(10, 699, 160, 3),        # left of Log Off
            foot_b=(10, 735, 160, 3),
        ),
    ),
)
L = LAYOUTS.get(VARIANT)
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


def edge(v):
    """An edge's lines (outer, middle..., inner, in either order), as 4."""
    if L["edge3"]:
        assert len(v) == 3
        return [v[0], v[1], v[1], v[2]]
    assert len(v) == 4
    return v


def measured_caption(f):
    """The caption's middle column and its 8 left and 5 right columns, as
    the reference shows them: (r, g, b), or None where the corner is cut."""
    x0, y0, x1 = f["x0"], f["y0"], f["x1"]
    rows = L["caption_rows"]
    mid = [None] * rows
    for (x, a, b) in f["mid"]:
        for r in range(a, b):
            mid[r] = pix(x, y0 + r)
    left, right = [], []
    for r in range(rows):
        lrow = []
        for c in range(8):
            cut = c < len(CORNER_CUT) and r < CORNER_CUT[c]
            p = pix(x0 + c, y0 + r)
            assert not cut or L["desktop"] is None or p == L["desktop"], ("corner", r, c, p)
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
    e = L["extra_row"]
    return rows if e is None else rows[:e] + [rows[e]] + rows[e:]


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
    glyph) on the flat buttons. Close's face shades across the row, so
    there the glyph is its white X and the outline darker than the row's
    median (the thin X never fills a row). Where the glyphs are plain white
    on a face shaded both ways (Olive Green), they are the white pixels."""
    face = [[pix(x + i, y + j) if j < rows else None for i in range(21)] for j in range(21)]
    glyph = {}
    lo, hi = GLYPH_SPAN
    for j in range(lo, min(hi + 1, rows)):
        row = sorted((face[j][i] for i in range(1, 20)), key=luma)
        med = row[len(row) // 2] if close else face[j][17]
        for i in range(lo, hi + 1):
            p = face[j][i]
            far = max(abs(p[c] - med[c]) for c in range(3))
            if L["white_glyphs"]:
                on = min(p) >= 0xF0
            elif close:
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


def frame_tables_measured(tag, f, active_faces=None, active_glyphs=None):
    mid, left, right = measured_caption(f)
    assert all(p is not None for p in mid)
    mid, left, right = with_extra_row(mid), with_extra_row(left), with_extra_row(right)
    x0, x1, y1 = f["x0"], f["x1"], f["y1"]
    n = 3 if L["edge3"] else 4
    side_l = edge([pix(x0 + c, f["side_y"]) for c in range(n)])
    side_r = edge([pix(x1 - n + 1 + c, f["side_y"]) for c in range(n)]) if f["side_right"] \
        else list(reversed(side_l))
    bottom = edge([pix(f["bottom_x"], y1 - n + 1 + r) for r in range(n)])
    bl = edge([edge([pix(x0 + c, y1 - n + 1 + r) for c in range(n)]) for r in range(n)])
    br = edge([edge([pix(x1 - n + 1 + c, y1 - n + 1 + r) for c in range(n)])
               for r in range(n)]) if f["corner_br"] else [list(reversed(r)) for r in bl]
    pre = f"luna_{VARIANT}_{tag}"
    out = [
        f"/* {tag} caption: middle column, top to bottom. */",
        table(f"{pre}_cap_mid[{CAPTION_H}]", [hexc(p) for p in mid]),
        f"/* {tag} caption: the 8 left and 5 right columns (alpha 0 = cut corner). */",
        table(f"{pre}_cap_left[{CAPTION_H}][8]",
              drop_window_icon([[hexo(p) for p in r] for r in left])),
        table(f"{pre}_cap_right[{CAPTION_H}][5]", [[hexo(p) for p in r] for r in right]),
        f"/* {tag} borders: left: outer first; right: inner first; bottom: top row first. */",
        table(f"{pre}_side_left[{BORDER}]", [hexc(p) for p in side_l]),
        table(f"{pre}_side_right[{BORDER}]", [hexc(p) for p in side_r]),
        table(f"{pre}_bottom[{BORDER}]", [hexc(p) for p in bottom]),
        table(f"{pre}_corner_bl[{BORDER}][{BORDER}]", [[hexc(p) for p in r] for r in bl]),
        table(f"{pre}_corner_br[{BORDER}][{BORDER}]", [[hexc(p) for p in r] for r in br]),
    ]
    faces, glyphs = {}, {}
    for name in ("minimize", "maximize", "close", "help"):
        if name in f["buttons"]:
            bx, by = f["buttons"][name]
            face, glyph = measure_button(bx, by, f["button_rows"], name == "close")
            if f["button_rows"] < 21:
                fill_hidden_rows(face, f["button_rows"], active_faces[name])
        elif active_glyphs is not None and name in active_glyphs:
            # Help on an inactive window: the Minimize face; the active
            # glyph in the inactive glyph colours, matched pixel for pixel
            # on Minimize and Maximize.
            face = [list(r) for r in faces["minimize"]]
            ink = {}
            for other in ("minimize", "maximize"):
                for pos, p in active_glyphs[other].items():
                    if pos in glyphs[other]:
                        ink.setdefault(p, glyphs[other][pos])
            glyph = {pos: ink[p] for pos, p in active_glyphs[name].items()}
        else:
            continue  # not shown: luna.c draws it on the Minimize face
        faces[name], glyphs[name] = face, glyph
        out.append(f"/* {tag} {name} button: the face (its glyph painted out; alpha 0 = cut"
                   " corner), then the glyph ('.': none; letters index the ink). */")
        out.append(face_table(f"{pre}_{name}", face))
        out += glyph_tables(f"{pre}_{name}", glyph)
    # For glyphs the reference doesn't show (Restore, maybe Help):
    # Maximize's outline, title bar fill and light edge (on a flat glyph,
    # all its one colour).
    g = glyphs["maximize"]
    outline = g[(5, 5)]
    out.append(f"/* {tag} glyph colours: outline, fill, light edge. */")
    out.append(table(f"{pre}_glyph_colors[3]",
                     [hexc(outline), hexc(g.get((6, 6), outline)), hexc(g.get((4, 6), outline))]))
    return "\n".join(out), faces, glyphs


def bar_tables_measured():
    b = L["bar"]
    y0 = b["y"]
    pre = f"luna_{VARIANT}"

    def block(name, xs, rows, top):
        lo, hi = xs
        return table(f"{name}[{rows}][{hi - lo}]",
                     [[hexc(pix(x, top + r)) for x in range(lo, hi)] for r in range(rows)])

    def col(name, x, rows, top):
        return table(f"{name}[{rows}]", [hexc(pix(x, top + r)) for r in range(rows)])

    out = ["/* Taskbar, top to bottom. */", col(f"{pre}_taskbar", b["x"], 30, y0)]
    # Normal task buttons are measured unless the reference shows none.
    out.append("/* Start button: left columns, then right columns (the text and logo"
               " area between is interpolated). */")
    out.append(block(f"{pre}_start_left", b["start"]["left"], 30, y0))
    out.append(block(f"{pre}_start_right", b["start"]["right"], 30, y0))
    r0, r1 = TASK_ROWS
    for state in ("normal", "pressed"):
        t = b.get("task" if state == "normal" else "task_pressed")
        if t is None:
            continue
        out.append(f"/* Task button ({state}): left, middle and right columns. */")
        out.append(block(f"{pre}_task_{state}_left", t["left"], r1 - r0, y0 + r0))
        out.append(col(f"{pre}_task_{state}_mid", t["mid"], r1 - r0, y0 + r0))
        out.append(block(f"{pre}_task_{state}_right", t["right"], r1 - r0, y0 + r0))
    tr = b["tray"]
    out.append("/* Notification area: left edge, then the body at its left and right"
               " ends (interpolated between). */")
    out.append(block(f"{pre}_tray_left", tr["left"], 30, y0))
    out.append(col(f"{pre}_tray_a", tr["a"], 30, y0))
    out.append(col(f"{pre}_tray_b", tr["b"], 30, y0))
    p = L["push"]
    if p is not None:
        out.append("/* Push button, 75x23 with its ring: the five columns at each end"
                   " and the middle column, top to bottom. */")
        for name, (lo, hi) in (("left", p["left"]), ("right", p["right"])):
            out.append(table(f"{pre}_button_{name}[5][23]",
                             [[hexc(pix(x, p["y"] + r)) for r in range(23)]
                              for x in range(lo, hi)]))
        out.append(col(f"{pre}_button_mid", p["mid"], 23, p["y"]))
        out.append("/* A disabled push button: border and face. */")
        out.append(f"static const uint32_t {pre}_button_disabled_border = "
                   f"{hexc(pix(*p['disabled_border']))};")
        out.append(f"static const uint32_t {pre}_button_disabled_face = "
                   f"{hexc(pix(*p['disabled_face']))};")
    return "\n".join(out)


def startmenu_tables():
    """The Start menu's colours, from the second screenshot, by region:
    the mean, or for a body the commonest colour (the palette's dither
    leaves other colours around its text and icons)."""
    global REF, _PIX
    REF, _PIX = START_REF, None
    out = ["/* Start menu: averaged over regions of a palette-quantized screenshot. */"]
    for name, (x, y, w, h) in L["startmenu"].items():
        px = [pix(x + i, y + j) for j in range(h) for i in range(w)]
        if name in ("left_bg", "right_bg", "column_edge", "left_sub"):
            counts = {}
            for p in px:
                counts[p] = counts.get(p, 0) + 1
            ranked = sorted(counts, key=lambda p: -counts[p])
            # The subtitle: the commonest colour that isn't the background.
            c = ranked[1] if name == "left_sub" else ranked[0]
        else:
            c = tuple(round(sum(p[k] for p in px) / len(px)) for k in range(3))
        out.append(f"static const uint32_t luna_{VARIANT}_startmenu_{name} = {hexc(c)};")
    return "\n".join(out)


def main_measured():
    guard = f"XD_THEME_LUNA_DATA_{VARIANT.upper()}_H"
    print(f"""/* SPDX-License-Identifier: MIT
 * Generated by tools/luna-measure.py --variant {VARIANT} from a Windows XP
 * {L["title"]} screenshot: colour profiles of its frame, caption buttons
 * and taskbar, as luna_data.h has Blue's. Colours only (0xAARRGGBB);
 * shapes and text are drawn by luna.c. Do not edit by hand.
 */
#ifndef {guard}
#define {guard}

#include <stdint.h>
""")
    text, faces, glyphs = frame_tables_measured("active", L["active"])
    print(text)
    print()
    if L["inactive"] is not None:
        text, _, _ = frame_tables_measured("inactive", L["inactive"], faces, glyphs)
        print(text)
        print()
    print(bar_tables_measured())
    if "startmenu" in L:
        print()
        print(startmenu_tables())
    print("\n#endif")


if VARIANT == "blue":
    main_blue()
else:
    main_measured()
