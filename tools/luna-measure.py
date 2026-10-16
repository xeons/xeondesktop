#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measures the Luna (Windows XP) look from a 96 DPI screenshot and writes
src/theme/luna_data.h: colour tables for the window frame, caption buttons
and taskbar that style_modern.c draws from.

    tools/luna-measure.py SCREENSHOT.png > src/theme/luna_data.h

Only colours are taken: per-row/per-column gradient profiles, edge lines,
corner masks, and smooth polynomial surfaces fitted to the caption buttons.
Glyphs, text and icons are drawn by the theme engine itself.

The coordinates below are where each element sits in the reference
screenshot (an active Notepad window, an inactive Explorer window and the
taskbar of a stock Windows XP SP3 desktop at 1920x972). Needs ImageMagick.
"""
import re
import subprocess
import sys

REF = sys.argv[1]

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
