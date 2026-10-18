/* SPDX-License-Identifier: MIT
 * The Qt palette that goes with our generated GTK theme.
 *
 * Qt's "gtk3" platform theme builds its palette from a few GTK widget
 * colours, and not the ones Windows' palette needs: Base (behind lists
 * and text) comes out as the dialog colour, and the bevel colours are
 * worked out from it rather than taken from the scheme. Qt 6 lets a JSON
 * file stand in for that mapping (QT_GUI_GTK_JSON); this writes one from
 * the appearance, in the format Qt itself saves (QT_GUI_GTK_JSON_SAVE).
 * Qt 5's GTK theme has no such file and keeps its own colours.
 */
#ifndef XD_SETTINGS_QTPALETTE_H
#define XD_SETTINGS_QTPALETTE_H

#include "../theme/theme.h"

/* The palette file's name, beside gtk-3.0 in the generated theme. */
#define XD_QT_PALETTE_FILE "qt6-palette.json"

/* The JSON for theme `t`'s colours. */
void xd_qt_palette_json(const xd_theme *t, xd_strbuf *out);

#endif
