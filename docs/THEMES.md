# Theme packages (.xdtheme)

Status: the format and `xd-theme-import` exist; the theme engine doesn't
draw packages yet (window frames and controls come first, then the
taskbar and Start menus, then Explorer's panes).

A theme package styles the whole desktop: window frames and controls (and
through the generated GTK theme, GTK programs), the taskbar, the Start
menus, Explorer's panes, and optionally the wallpaper, sounds and cursors.
It is a zip file named `NAME.xdtheme`, or the same files unpacked in a
folder `~/.local/share/xeondesktop/themes/NAME/` (for authoring; a folder
wins over a zip of the same name).

Windows visual styles (`.msstyles`, with their `.theme` file and
`shellstyle.dll`) are not read by the desktop: `xd-theme-import` converts
them into a package once (Display Properties runs it when one is chosen).
The built-in styles (Classic, Luna, Whistler, Aero) stay drawn in code; a
package draws only what it defines, and everything else falls back to the
built-in style it names as its base.

## Vocabulary

Packages use the vocabulary of Windows' uxtheme, so an imported msstyles
loses nothing and anyone who has edited one already knows it:

- **class** — a control or shell element: `Window`, `Button`, `ScrollBar`,
  `TaskBar`, `StartPanel`, `ExplorerBar`... An application-specific
  variant is `App::Class` (`TaskBand::Toolbar`, `StartMenu::Toolbar`), as
  in msstyles; it's used where the application asks for it and falls back
  to `Class`.
- **part** and **state** — by name, as Microsoft documents them ("Parts
  and States"): `Window` / `Caption` / `Active`, `Button` / `PushButton` /
  `Hot`. A number is accepted where no name exists.
- **property** — uxtheme's property names ("Property Identifiers"):
  `BgType`, `ImageFile`, `ImageCount`, `ImageLayout`, `SizingMargins`,
  `SizingType`, `ContentMargins`, `TextColor`, `GlyphImageFile`...
- Classes, parts, states and properties are matched case-insensitively.
- A property not found for a part in a state is looked up for the part,
  then the class, then (for `App::Class`) in `Class`, then in
  `ClassStyle` if the package has one (Vista and 7 keep their shared
  styling in `ButtonStyle`, `WindowStyle`...), as uxtheme does.

Our own additions, for what uxtheme never covered, are named the same
way, with names no Windows style uses: `ClassicStartMenu` (the Windows
2000 Start menu and its banner), `JumpList`, `TaskbarThumbnail`,
`DesktopIcons` (icon text).

## Layout

    theme.xml                 the package: name, base style, variants
    styles/NAME.xml           one per variant (colour x size)
    images/...                PNG, straight alpha
    shell/NAME.xml            Explorer's panes for a variant (optional)
    wallpaper/, sounds/, cursors/   (optional, from a .theme file)

## theme.xml

```xml
<xdtheme version="1" name="Luna" base="luna">
  <author>Microsoft Design Team</author>
  <description>Imported from luna.msstyles</description>
  <variant id="blue" name="Default (blue)" size="normal" style="styles/blue.xml"
           shell="shell/blue.xml"/>
  <variant id="blue-large" name="Default (blue)" size="large" style="styles/blue-large.xml"
           shell="shell/blue.xml"/>
  <variant id="silver" name="Silver" size="normal" style="styles/silver.xml"
           shell="shell/silver.xml"/>
  <!-- From a .theme file; each optional. -->
  <wallpaper file="wallpaper/bliss.jpg" style="stretch"/>
  <sounds scheme="sounds/scheme.ini"/>
  <cursors scheme="cursors/scheme.ini"/>
</xdtheme>
```

`base` is the built-in style drawing whatever the package leaves out
(`classic`, `luna`, `whistler`, `aero`). Display Properties lists the
variants by `name` (Color scheme) and `size` (Font size), as XP's
Appearance tab does.

## styles/NAME.xml

```xml
<style>
  <!-- XP's [SysMetrics]: the colours, fonts and sizes every control uses
       (and that GTK and Qt get). -->
  <sysmetrics>
    <color name="ActiveCaption" value="0 84 227"/>
    <color name="ButtonFace" value="236 233 216"/>
    <font name="CaptionFont" face="Trebuchet MS" size="10" weight="bold"/>
    <size name="CaptionBarHeight" value="25"/>
    <bool name="FlatMenus" value="true"/>
  </sysmetrics>

  <!-- Properties for a class, a part, or a part in a state: the most
       specific match wins, as in uxtheme. -->
  <class name="Button">
    <part name="PushButton" BgType="ImageFile" ImageFile="images/blue/button.png"
          ImageCount="5" ImageLayout="Vertical" SizingMargins="8 8 9 9"
          SizingType="Stretch" ContentMargins="3 3 3 3" TextColor="0 0 0"/>
    <part name="PushButton" state="Disabled" TextColor="161 161 146"/>
    <part name="CheckBox" BgType="ImageFile" ImageCount="12" SizingType="TrueSize"
          ImageFile1="images/blue/checkbox13.png" MinDpi1="96"
          ImageFile2="images/blue/checkbox16.png" MinDpi2="118"/>
  </class>
  <class name="Window">
    <part name="Caption" ImageFile="images/blue/frametop.png" ImageCount="2"
          SizingMargins="12 12 0 0" ContentMargins="..." />
  </class>
</style>
```

Values are written as in msstyles: colours `R G B` (or `#RRGGBB`),
margins `left right top bottom`, enums by name (`ImageFile`, `Stretch`,
`TrueSize`, `Vertical`...), booleans `true`/`false`, file names relative to
the package.

## What the renderer does with them

As uxtheme's DrawThemeBackground and DrawThemeText do:

- `BgType`: `ImageFile` (9-slice of an image strip), `BorderFill` (a
  border and fill from `BorderSize`, `BorderColor`, `FillColor`,
  gradients), `None`.
- The image for a state is frame `state - 1` of `ImageCount`, laid out
  `Vertical` or `Horizontal`; `ImageFile1..5` with `MinDpi1..5` /
  `MinSize1..5` choose by DPI or size (`ImageSelectType`).
- `SizingMargins` and `SizingType` (`Stretch`, `Tile`, `TrueSize` with
  `TrueSizeStretchMark` and `UniformSizing`); `BorderOnly`; `MirrorImage`.
- Glyphs: `GlyphImageFile`, `GlyphType`, `GlyphTransparent` (the caption
  buttons' symbols, the scroll arrows).
- Text: `TextColor`, `TextShadowColor` / `Offset` / `Type`, `ContentMargins`,
  `CaptionMargins`, `ContentAlignment`.
- An `AtlasRect` takes the part's image from a rectangle of one larger
  image (Windows 7's DWM frame).
- Images are straight-alpha PNG: an import bakes `TransparentColor`
  (magenta) and XP's premultiplied 32-bit bitmaps into alpha.

## Scaling images for high DPI

Theme images are drawn for 96 DPI. At a higher scale (per monitor, as the
compositor sets it), the renderer picks, in this order:

1. **An image made for it**: a part's `ImageFile1..5` with `MinDpi1..5`
   (XP's checkboxes have 13, 16 and 25 pixel versions) is used unscaled
   when one matches the monitor's DPI, as uxtheme does.
2. **Otherwise the image is scaled**, with the filter the user chose (or
   the package's default):
   - `Nearest`: hard pixels.
   - `Integer`: whole-number scales only (200%, 300%), each pixel becomes
     an exact 2x2 or 3x3 block, so the result looks exactly like the
     original, only bigger. At other scales it scales up by the next whole
     number and then down smoothly (an "integer prescale", as emulators
     do), so pixels stay as even as they can.
   - `Smooth`: bilinear (cairo's GOOD filter).
   - Pixel-art scalers, as emulators use for smoother edges on small
     images: `Scale2x`, `Scale3x`, `Scale4x` (EPX: Scale2x twice), `2xSaI`,
     `Super2xSaI` and `SuperEagle` (2x; applied twice for 4x), written
     from their published descriptions. At a scale they don't do exactly
     (1.5x, or 3x for the 2x-only ones), they scale up past it and then
     down smoothly, as with `Integer`.

Scaling is done once per image and scale and cached, and on the **whole
strip before it's cut** into its 9 slices and states: a scaler working on
each slice alone would smear the slices' edges into each other. The
`SizingMargins`, `ContentMargins` and other lengths are multiplied by the
same scale.

A package may name its preferred filter (pixel-art themes look best with
`Integer` or a pixel-art scaler, photographic ones with `Smooth`):

```xml
<xdtheme ...>
  <scaling filter="Integer"/>
</xdtheme>
```

and the user's choice overrides it: Display Properties > Appearance >
Effects, "Scale theme images:" (Automatic — the package's, Nearest,
Integer, Smooth, Scale2x, Scale3x, Scale4x, 2xSaI, Super2xSaI,
SuperEagle) with "Use whole-number scaling when the scale
is a whole number" (on by default), saved in `desktop.ini`'s
`[Appearance]` as `ImageScaling` and `IntegerScaling`.

## Which parts draw what

| Desktop element | Classes |
|---|---|
| Window frames, caption buttons | `Window` (Aero glass: `DWMWindow`) |
| Push, check and radio buttons, group boxes | `Button` |
| Text boxes, drop-downs | `Edit`, `ComboBox` |
| Scroll bars | `ScrollBar` |
| Tabs | `Tab` |
| Progress, sliders, spin buttons | `Progress`, `TrackBar`, `Spin` |
| Lists, trees, column headers | `ListView`, `TreeView`, `Header` |
| Toolbars, rebars, status bars, tooltips | `Toolbar`, `Rebar`, `Status`, `Tooltip` |
| Menus (Vista/7) | `Menu` |
| Taskbar, task buttons, tray, clock | `TaskBar`, `TaskBand`, `TaskBand2`, `TrayNotify*`, `TrayNotify::Clock` |
| Start button | `Start::Button` (XP), `StartPanel`/`TaskBar` parts (7) |
| XP and 7 Start menus | `StartPanel`, `StartPanelPriv`, `StartMenu::*` |
| Windows 2000 Start menu | `ClassicStartMenu` |
| Explorer's task pane (XP) | `ExplorerBar` + `shell/NAME.xml` |
| Explorer's navigation and command bars (7) | `Navigation`, `CommandModule`, `PreviewPane`, `ReadingPane` |
| Jump lists, thumbnails | `JumpList`, `TaskbarThumbnail` |
| GTK programs | generated from the above (CSS `border-image`) |

## shell/NAME.xml

Explorer's panes beyond what the msstyles classes give: XP's task pane
(header and group colours, fonts, the "blockade" banners) from
`shellstyle.dll`'s stylesheet, as properties on `ExplorerBar` parts and
our own element names. To be specified with the Explorer phase.
