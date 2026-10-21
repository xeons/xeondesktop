# Theme packages (.xdtheme)

Status: the format and `xd-theme-import` exist; the theme engine draws
window frames and the common controls from a package, and Display
Properties chooses and imports packages (see "Choosing a package"), and
in its Themes tab applies their wallpaper, sounds and pointers too (see
"Wallpaper, sounds and pointers"). The
rest of the controls, the taskbar and Start menus, and Explorer's panes
are still drawn by the base style.

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

## Choosing a package

Packages are found in `$XDG_DATA_HOME/xeondesktop/themes` (that is,
`~/.local/share/xeondesktop/themes`), then in each `$XDG_DATA_DIRS`'
`xeondesktop/themes` (`/usr/share/xeondesktop/themes`): the first
directory holding a package of a name wins, and in it a folder `NAME/`
wins over `NAME.xdtheme`.

`desktop.ini`'s `[Appearance]` chooses one:

```ini
[Appearance]
Style=luna              ; the package's base, which Display Properties sets with it
ThemePackage=royale     ; the folder's name or the .xdtheme's stem; empty: none
ThemeVariant=royale-normal  ; a variant's id; empty or unknown: the first
ImageScaling=Automatic  ; or Nearest, Integer, Smooth, Scale2x, Scale3x, Scale4x,
                        ; 2xSaI, Super2xSaI, SuperEagle
IntegerScaling=true
```

Display Properties writes these: Appearance's "Windows and buttons" lists
the built-in styles and then the installed packages by their names
(theme.xml's), "Color scheme" the chosen package's colours and "Font
size" the sizes its colour comes in, as XP's Appearance tab does for a
visual style; the preview draws with the chosen package before Apply.
Browse... beside "Windows and buttons" (and the last item of the Themes
tab's list, as in XP, which then chooses it as a theme) takes a `.msstyles` or a `.theme`, which `xd-theme-import`
converts in the background into `~/.local/share/xeondesktop/themes/NAME.xdtheme`,
or a `.xdtheme`, which is copied there; either is then chosen.

The compositor, the shell, libxdui's programs and the lock screen load the
chosen package and follow `desktop.ini` as it changes: a new package (or
variant, or the same package rewritten) is loaded, everything is redrawn
with it, and only then is the old one freed. While High Contrast is on no
package is used (it isn't forgotten: it comes back with High Contrast
off), as Windows turns visual styles off for it.

For testing, `$XD_THEME_PACKAGE` (a path to a `.xdtheme` or folder) is
used instead of the settings' package, with `$XD_THEME_VARIANT` and
`$XD_IMAGE_FILTER`.

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
- Parts Microsoft never documented (Vista's and 7's shell classes) are
  named where their images and use make them clear, in
  data/theme/uxtheme.ini's `[Deduced Class Version]` sections (per Windows
  version, since the numbers changed between Vista and 7); the rest stay
  numbered, with a note in the file saying what they look like. So far:
  - DWMWindow: the caption buttons, glows and glyphs (one per DPI:
    `CloseGlyph96` ... `CloseGlyph192`), the small close button, the
    title's glow (`CaptionTextGlow`); the frame's edges (`FrameTop`,
    `FrameBottom`, `FrameLeft`, `FrameRight`: see "Vista's and 7's glass
    frame"), the drop shadow (`ShadowTop`, `ShadowBottom`, `ShadowLeft`,
    `ShadowRight`) and the reflections over the glass
    (`ReflectionStreaks`, `ReflectionTopLeft`, Vista's
    `ReflectionTopRight`). Still numbered: the other sets of edge pieces
    (a softer set, 7's without a client edge or with rounder corners, and
    square-cornered ones), a strip of grey bands and 7's two animations.
  - 7's taskbar buttons and progress bars (TaskBand2), thumbnails
    (TaskbandExtendedUI), Start menu (StartPanelPriv, with StartPanel's
    names where it has StartPanel's images: `MoreProgramsTab`, `NscHost`,
    `LogoffSplitButtonDropdown`...). See "The taskbar and Start menus".
  - Explorer's panes: `CommandModule`, `PreviewPane`, `InfoBar` and
    `SearchHome` (named as Vista's own `shellstyle.dll` stylesheet names
    them: `CMOD_TASKBUTTON` is `TaskButton`), `ReadingPane`, the one part
    of `AddressBand`, `BreadcrumbBar`, `SearchBox` and `HelpSearchBox`
    (`Background`), and 7's navigation pane, `ProperTree` (`Header`, the
    Favorites and Libraries headings; `Background`), as 7's stylesheet
    uses them.
  - `AltTab` (`HotItem`, `SelectedItem`, `HotSelectedItem`, `Title`,
    `ThumbnailShadow`; the class's own image is the box), `TextGlow`
    (`Glow`) and 7's notification area overflow, `TrayNotifyFlyout`
    (`Window`, `LinkArea`, as Flyout's documented parts).
  - Still numbered: 7's `ItemsView` (search results' Content view),
    `TryHarder` ("Search again in:"), `DropListControl`, `SearchEditBox`
    and Vista's `QueryBuilder`, whose use the images don't make clear.

  Some classes Vista's and 7's SDK leave out are documented in its
  vssym32.h (Wine's has them), and are in the regular sections: `Link`,
  `EmptyMarkup`, `Static`, `MonthCal` (the calendar) and StartPanel's
  parts 12 to 19 (`MoreProgramsTab` ... `LogoffSplitButtonDropdown`, the
  arrow beside 7's Shut down button).
- A property not found for a part in a state is looked up for the part,
  then the class, then (for `App::Class`) in `Class`, then in
  `ClassStyle` if the package has one (Vista and 7 keep their shared
  styling in `ButtonStyle`, `WindowStyle`...), as uxtheme does.

Our own additions, for what uxtheme never covered, are named the same
way, with names no Windows style uses: `ClassicStartMenu` (the Windows
2000 Start menu and its banner), `JumpList`, `DesktopIcons` (icon text).

## Layout

    theme.xml                 the package: name, base style, variants
    styles/NAME.xml           one per variant (colour x size)
    images/...                PNG, straight alpha
    shell/NAME.xml            Explorer's panes for a variant (optional)
    wallpaper/NAME            the wallpaper (optional; these three from a .theme file)
    sounds/scheme.ini, *.wav  a sound scheme and its sounds
    cursors/                  an XCursor theme: index.theme, cursors/NAME

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
  <cursors theme="cursors"/>
</xdtheme>
```

`base` is the built-in style drawing whatever the package leaves out
(`classic`, `luna`, `whistler`, `aero`). Display Properties lists the
variants by `name` (Color scheme) and `size` (Font size), as XP's
Appearance tab does.

## Wallpaper, sounds and pointers

What XP's Themes tab sets besides the visual style. Each is optional; a
package with any of them is listed in Display Properties' Themes tab (one
with only a style is listed in Appearance alone).

- `<wallpaper file="..." style="...">`: a picture in the package, placed
  as `[Desktop]`'s `WallpaperMode` names it (`center`, `tile`, `stretch`,
  `fill`, `fit`; `stretch` if left out, as XP's .theme files default to).
- `<sounds scheme="sounds/scheme.ini">`: a sound scheme in the format of
  `sounds.ini` (src/settings/sounds.h), except that a sound file is a path
  in the package:

  ```ini
  [Sounds]
  Scheme=default            ; events not listed keep Windows Default's (or "none")
  [Events]
  WindowsLogon=sounds/Logon.wav   ; a file in the package
  CriticalStop=dialog-error       ; a sound from the XDG sound theme, by name
  Maximize=                       ; no sound
  ```

- `<cursors theme="cursors">`: a folder of the package holding an
  XCursor theme as libXcursor reads one: `index.theme` (`[Icon Theme]`
  `Name`, `Inherits=default` for the pointers it lacks) and
  `cursors/NAME`, an XCursor file per pointer name (`left_ptr`, `wait`,
  `text`, `pointer`...). A zip holds no links: the aliases are copies.

Choosing a theme in the Themes tab unpacks these where the settings can
point at them: the wallpaper and sounds into
`$XDG_DATA_HOME/xeondesktop/theme-files/NAME/` (at their paths in the
package), the pointers into `$XDG_DATA_HOME/icons/xdtheme-NAME/`. Apply
then writes `[Appearance]` `ThemePackage` and `ThemeVariant` (its first
variant) with `Style` its base, `[Desktop]` `Wallpaper` and
`WallpaperMode`, `[Mouse]` `CursorTheme=xdtheme-NAME` and the sound scheme
into `sounds.ini`. What a package doesn't bring is left as it is, as with
XP's .theme files. The tab's built-in "Windows XP" (Luna, Default (blue),
our Rolling Hills for Bliss, stretched, Windows' own pointers) and
"Windows Classic" (no wallpaper) set the sounds not at all, as XP's own
.theme files didn't.

Save As... writes the current settings as a package: the chosen package's
style and variants (the chosen one first), or an empty style over the
built-in one when there is none; the wallpaper, the sound scheme with its
sound files copied in, and the pointer theme's cursors (links followed).

`xd-theme-import` takes a .theme's extras from beside it, found by file
name (Windows' own `%SystemRoot%\Media` and `\Cursors` aren't there):

- the wallpaper (`[Control Panel\Desktop]`);
- the sounds of `[AppEvents\Schemes\Apps\.Default\EVENT\.Current]` and
  `...\Explorer\EmptyRecycleBin\.Current` (`DefaultValue`), Windows'
  event names mapped to ours (`SystemHand` is `CriticalStop`, `MailBeep`
  `NewMail`, `AppGPFault` `ProgramError`...); an empty value is no sound,
  a file not beside the theme keeps Windows Default's;
- the pointers of `[Control Panel\Cursors]` (`Arrow`, `Help`,
  `AppStarting`, `Wait`, `Crosshair`, `IBeam`, `NWPen`, `No`, `SizeNS`,
  `SizeWE`, `SizeNWSE`, `SizeNESW`, `SizeAll`, `UpArrow`, `Hand`), .cur
  and .ani converted to XCursor files with
  [win2xcur](https://github.com/quantum5/win2xcur) under the XCursor
  names toolkits ask for (`Arrow` is `left_ptr`, `default`, `arrow`...).
  Without win2xcur the import goes on without the pointers and says so.

## styles/NAME.xml

```xml
<style>
  <!-- XP's [SysMetrics]: the colours, fonts and sizes every control uses
       (and that GTK and Qt get). MsgBoxFont is read but not used: dialogs
       keep the dialog font (Tahoma 8), as Windows' do under 7's Segoe UI 9
       and XP's Large Fonts. -->
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
  `Vertical` or `Horizontal` (uxtheme's default: XP's size grips name
  none; an `AtlasRect`'s frames are stacked); `ImageFile1..5` with `MinDpi1..5` /
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
  (magenta) and XP's premultiplied 32-bit bitmaps into alpha, and makes
  Vista's and 7's PNGs, which hold premultiplied colour, straight.

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
| Alt+Tab | `AltTab` (Vista, 7; Windows 2000 and XP never themed it) |
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
| Explorer's command bar, address box, tree and details pane (Vista, 7) | `CommandModule`, `AB::AddressBand`, `Explorer::TreeView`, `PreviewPane` |
| Jump lists, thumbnails | `JumpList` (else 7's `StartPanelPriv` items), `TaskbandExtendedUI` |
| GTK programs | generated from the above (CSS `border-image`) |

## GTK and Qt programs

The generated GTK 3 theme (`~/.local/share/themes/XeonDesktop`) draws GTK
widgets with the package's images, as the XP-styled GTK themes of old did
(src/theme/gtkpkg.c):

- Each part and state a widget needs is drawn once by the renderer above,
  at its image's own size, into `gtk-3.0/assets/pkg/NAME.png` and
  `NAME@2.png` (at 200%, with the image filter and DPI-specific images, as
  on screen).
- Stretched parts become a CSS `border-image` cut at the part's
  `SizingMargins` (`repeat` for `Tile`), so GTK keeps the corners and
  stretches the edges as uxtheme does. GTK never fills a border image's
  middle, so the middle is the image's inside as the background. The CSS
  border (where the content starts) is the `SizingMargins` along a
  widget's flexible side and the `ContentMargins` across it (a 23-pixel
  button around 9-pixel margins).
- `TrueSize` parts are shown at their size: check boxes and radio buttons
  as GTK's icon, scroll arrows and slider thumbs as backgrounds.
- `BorderFill` parts become a CSS border and fill.

| GTK widget | Parts |
|---|---|
| Push buttons (normal, hot, pressed, disabled, default/focused) | `Button` `PushButton` |
| Check boxes, radio buttons (all 12 and 8 states) | `Button` `CheckBox`, `RadioButton` |
| Group boxes (`GtkFrame`) | `Button` `GroupBox` |
| Entries, spin buttons' fields | `Edit` `EditText` |
| Combo boxes | `ComboBox`, its `DropDownButton` with its glyph |
| Scroll bars | `ScrollBar` `ArrowBtn`, `ThumbBtn*` with `Gripper*`, `UpperTrack*` |
| Notebooks (tabs on top) | `Tab` `TabItem` (`TabItemLeftEdge` first, `TopTabItem` selected), `Pane`, `Body` |
| Progress bars | `Progress` `Bar`, `Chunk` (`ProgressChunkSize`, `ProgressSpaceSize`) |
| Scales | `TrackBar` `Track`, `Thumb` (`ThumbBottom`/`ThumbTop` with marks on one side) |
| Spin buttons | `Spin` `Up`, `Down` with their glyphs |
| Tree view headers | `Header` `HeaderItem` |
| Tooltips | `Tooltip` `Standard` |

Its `[SysMetrics]` colours (and the menu font) replace the base style's
throughout the CSS: the dialog face, window and text colours, selection,
tooltips, menus (`MenuHilight` and `MenuBar` with `FlatMenus`), and the Qt
palette written beside it. A widget the package doesn't style keeps the
base style's generated CSS.

## The taskbar and Start menus

Drawn from the package where it styles the part, else as the base style
draws it. With desktop composition (Vista's and 7's glass), a class's
`...Composited::` variant is used where it has its own image.

- **Taskbar**: `TaskBar` (7: `TaskBar2::TaskBar`) `BackgroundBottom`,
  `Top`, `Left`, `Right` by the edge it's on, over the glass colour for
  Vista and 7; `SizingBar...` along the edge facing the desktop while the
  taskbar is unlocked.
- **Start button**: XP's `Start::Button` (Normal, Hot, Pressed), with our
  logo and "start" in its `Font`, `TextColor` and text shadow (XP's flag
  is explorer.exe's, not the style's); Vista's orb, `StartMiddle::Button`.
  7's orb isn't in its style: ours is drawn.
- **Task buttons** (XP): `TaskBand::Toolbar` (`TaskBandVert::Toolbar` on
  a side taskbar) `Button` in toolbar states (the active window's is
  Checked); `TaskBand` `FlashButton` while one flashes, `GroupCount`'s
  font and colour for a group's count.
- **7's program buttons**: `TaskBand2` `AppButton` (frames 1-3 a
  background window's Normal, Hot, Pressed; 4-6 the active one's; 7
  flashing), `AppButtonStack` / `AppButtonStackLit` for a group's other
  windows (frames for none, one, two), and the progress bars
  `ProgressNormal`, `ProgressPaused`, `ProgressError`, `ProgressPulse`.
- **Notification area**: `TrayNotifyHoriz::TrayNotify` (`...Vert::` on a
  side taskbar) `Background`; `TrayNotify::Clock`'s `TextColor` and
  `Font`; the chevron `TrayNotifyHoriz::Button` (`...HorizOpen::` while
  the hidden icons show); 7's `ShowDesktop::Button`.
- **Thumbnails** (7): `TaskbandExtendedUI` `ThumbnailBackground` over the
  glass, `ThumbnailTile` (Hot 1, Pressed 2), `ThumbnailTitle`'s colour,
  `ThumbnailClose` (Normal, Hot, Pressed).
- **XP's Start menu**: `StartPanel` `UserPane` (and its `Font`,
  `TextColor`, text shadow for the name), `UserPicture`, `ProgList`
  (`TextColor`, `CaptionText` for a pinned program's second line),
  `ProgListSeparator`, `PlacesList`, `PlacesListSeparator`,
  `MorePrograms`, `MoreProgramsArrow`, `Logoff` (`TextColor`) and
  `LogoffButtons` (a strip of square icons: Log Off's is the next to
  last, Turn Off Computer's the last).
- **7's Start menu**: `StartPanelPriv`'s, then `StartPanel`'s, parts of
  the same names, and (deduced) `MoreProgramsArrowBack`, `ProgListItem`
  (a program's highlight; with a jump list `ProgListItemLeft` and
  `ProgListItemSplit`), `PlacesListItem`, `ShutdownButton`,
  `LogoffSplitButtonDropdown` (its arrow), `OpenBox` (the search box's
  pane under the program list, its `ContentMargins` placing the box, as
  Vista's part of that name; the list ends above it, its items within
  `ProgList`'s `ContentMargins`); search results' headings in
  `StartMenu::ListView` `GroupHeader`'s `Heading1TextColor`.
- **The Start menus' cascades** (All Programs, Recent Documents, Control
  Panel): `StartMenu::Toolbar` (its background, `Button` Hot for the hot
  item, the class's `TextColor` and Hot one), `StartMenu::MenuBand`
  `Seperator` (XP's spelling; `Separator` too) and `NewAppButton` (a
  newly installed program).
- **The Windows 2000 Start menu**, `ClassicStartMenu`, ours: `Background`
  (the menus), `Banner` (with its text's `TextColor` and `Font`), `Item`
  (Normal, Hot, Disabled, DisabledHot, and their `TextColor`),
  `Separator`, `SubmenuArrow` (Normal, Hot, Disabled). It styles the
  Start menu and every menu cascading from it.
- **Jump lists**, `JumpList`, ours: `Background`, `Band` (the bottom
  band with the program's own items), `Header` (`TextColor`), `Item`
  (Normal, Hot, Disabled, and their `TextColor`), `PinButton` (Normal,
  Hot, Pressed). Without them, a hot item or pin is 7's Start menu's
  `ProgListItem`, as 7 draws both with the same control.

## Vista's and 7's glass frame and Alt+Tab

With the glass on, Aero's frame takes from `DWMWindow`, where the package
has them: the caption buttons with their glows and glyphs, the
reflections laid over the glass colour from the window's top corners
(`ReflectionStreaks`, `ReflectionTopLeft`, `ReflectionTopRight`, as they
are, at the state's `Opacity`: 1 active, 2 inactive, 3 and 4 the same
maximized), and the edges. Each edge piece is two frames, active and
inactive:

- `FrameTop` (21 x 27): rows 0-6 are the top corners (columns 0-6 and
  14-20) and the top edge (column 10, stretched); rows 24-26 of columns
  8-12 the client edge's top corners and its top line (column 10).
- `FrameBottom` (21 x 11): rows 0-2 the client edge's bottom, rows 3-10
  the bottom corners and edge.
- `FrameLeft`, `FrameRight` (11 x 3): across a side, the outer edge (the
  three columns at the window's edge) and the client edge (the three at
  the client area's: a light line, a dark one, and one under the client
  area).

How DWM itself stretches them isn't documented (its `SizingMargins` would
squeeze the client edge's light line out of an 8 px border), so the outer
and client edges are each laid where their lines say, with the glass
between. The shadow parts aren't used: the compositor draws its own.

Alt+Tab's window is Windows 2000's and XP's in every style (a raised box,
the icons in rows of seven, a square round the selected one, its title in
a sunken box below), and from a package with `AltTab` 7's without glass:
the class's image as the box, the title above the icons in `Title`'s
colour and font on `TextGlow`'s glow, `SelectedItem` behind the selected
window's icon.

## shell/NAME.xml

Explorer's panes beyond what the msstyles classes give. A variant names it
with `shell="shell/NAME.xml"` (several variants may share one: XP's are per
colour scheme, not per size). It has the style's form, `<class>` and
`<part>` elements under a `<shell>` root, and is read after the style: its
properties override the style's where both have one (luna.msstyles has an
`ExplorerBar` of its own, for Internet Explorer's bars), and lookups fall
back as in the style.

XP's Explorer draws its task pane with DirectUI, from stylesheets in
`shellstyle.dll` (`Shell\<ColorScheme>\shellstyle.dll` beside the
msstyles: `Shell\NormalColor`, `Shell\Metallic`, `Shell\HomeStead`, and
Royale's `shell\royale`). `xd-theme-import` reads that DLL for each colour
scheme and writes what the task pane needs here; the bitmaps it uses become
PNGs (`images/shell/SCHEME/ID.png`, the magenta made clear). The stylesheet
format has no library: the importer has a small tokenizer and parser for
what XP's stylesheets contain.

The task pane, on uxtheme's `ExplorerBar` (its parts' meanings are
uxtheme's, so a Vista or 7 package's own `ExplorerBar` draws the pane too):

| Where | Properties |
|---|---|
| `ExplorerBar` (the class): the pane | `BgType BorderFill`, `FillType` `VertGradient` with `GradientColor1`/`2` or `Solid` with `FillColor`, `BorderColor`, `BorderThickness`, `ContentMargins` (around the groups), `GroupSpacing` |
| `NormalGroupHead`, `SpecialGroupHead`: a group's title bar | `ImageFile` (stretched with `SizingMargins`), `ContentMargins` (the title's), `Font`, `TextColor`; state `Hot`: `TextColor` |
| `NormalGroupExpand`, `NormalGroupCollapse` (and Special): the title bar's button | `ImageFile` per state (`Normal`, `Hot`, `Pressed`) or a strip with `ImageCount`; `Offset` from the title bar's top right (`OffsetType TopRight`; the button's right edge, or its left edge when that's further out than its width) |
| `NormalGroupBackground`, `SpecialGroupBackground`: a group's body | `BgType BorderFill` with `FillColor`, `BorderColor`, `BorderThickness`, or an image; `ContentMargins` (around its tasks); the tasks' `Font` and `TextColor`, and in state `Hot` theirs as the pointer is over one (XP underlines them: `Font="Tahoma, 8, Underline"`); `LinkSpacing` (above each task), `IconSpacing` (between a task's icon and its text) |

The special group is XP's darker one (My Computer's System Tasks, Control
Panel's). Our own names, which no Windows style uses:

- Properties: `BorderThickness` (a border's widths, `left right top
  bottom` as MARGINS: DirectUI's boxes have borders on some sides only;
  uxtheme's `BorderSize` is the same all round), `GroupSpacing`,
  `LinkSpacing`, `IconSpacing` (lengths at 96 DPI).
- `TaskPane` / `Details`: the Details group's lines (`Font`, `TextColor`,
  `LinkSpacing` above each); `TaskPane` / `DetailsName`: its first line,
  the item's name (`Font`).
- `Blockade`: XP's banner over system folders ("These files are hidden."):
  `Background` (a `BorderFill`), `Accent` (its gradient strip), `Title`,
  `Message` and `Link` (`Font`, `TextColor`, `ContentMargins`; `Link` in
  state `Hot`). Imported; nothing draws it yet.

```xml
<shell>
  <class name="ExplorerBar">
    <part BgType="BorderFill" FillType="VertGradient" GradientColor1="123 162 231"
          GradientColor2="99 117 214" BorderColor="255 255 255" BorderThickness="1 0 0 1"
          ContentMargins="13 12 12 13" GroupSpacing="15"/>
    <part name="NormalGroupHead" BgType="ImageFile" ImageFile="images/shell/normalcolor/112.png"
          SizingType="Stretch" SizingMargins="2 2 2 0" ContentMargins="12 3 2 0"
          Font="Tahoma, 8, Bold" TextColor="33 93 198"/>
    <part name="NormalGroupHead" state="Hot" TextColor="66 142 255"/>
    <part name="NormalGroupExpand" state="Normal" BgType="ImageFile" ImageCount="1"
          ImageFile="images/shell/normalcolor/102.png" SizingType="TrueSize"
          OffsetType="TopRight" Offset="-3 2"/>
    <part name="NormalGroupBackground" BgType="BorderFill" FillType="Solid"
          FillColor="214 223 247" BorderColor="255 255 255" BorderThickness="1 1 0 1"
          ContentMargins="13 13 10 11" Font="Tahoma, 8" TextColor="33 93 198"
          LinkSpacing="4" IconSpacing="6"/>
    <part name="NormalGroupBackground" state="Hot" TextColor="66 142 255"
          Font="Tahoma, 8, Underline"/>
  </class>
  <class name="TaskPane">
    <part name="Details" Font="Tahoma, 8" LinkSpacing="5"/>
    <part name="DetailsName" Font="Tahoma, 8, Bold"/>
  </class>
</shell>
```

Without a package, the task pane is laid out with XP's lengths and drawn in
code: Luna's colour schemes in XP's own colours, other styles in their
scheme's.

Vista's and 7's Explorer need no shell file: their panes are msstyles
classes. Where a package has them, xd-files (XP's layout) draws its toolbar
as 7's command bar (`CommandModule`: `ModuleGradient` under
`ModuleBackground`, `TaskButton` for a button under the pointer, pressed
or checked), its address box as `AB::AddressBand`'s `Background`, its
Folders tree's rows as `Explorer::TreeView`'s `TreeItem` (whole rows, as
7's navigation pane) and its status bar as `PreviewPane`'s
`PreviewBackground` (7's details pane, which has its place).
