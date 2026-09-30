[![rebol-gui](https://github.com/user-attachments/assets/6197970d-99b4-42f7-9ff1-2edf6c3b90e5)](https://github.com/Oldes/Rebol-GUI)


# Rebol/GUI extension

A minimal windowing extension for [Rebol3](https://github.com/Oldes/Rebol3),
built on the current extension ABI. It opens native windows, places native
controls in them, and reports what the user did.

It is a fresh start rather than a port of the old View sources: there is no
`gob!`, no compositor and no DRAW dialect. Custom drawing goes through the
image widget - render into an `image!` with whatever draws pixels
([Blend2D](https://github.com/Siskin-framework/Rebol-Blend2D) being the
intended one) and `redraw` it.

Requires Rebol **3.22.10** or newer. Implementation notes for contributors are
in [INTERNALS.md](INTERNALS.md).

## What it is not (yet)

- no DRAW dialect, no compositor - just the image widget
- keyboard events only per window, with `keys?`, and observed rather than taken
- no checkable menu items
- nineteen native controls: button, image, text, field, area, check, radio,
  toggle, slider, progress, drop-list, drop-down, text-list, date-field, panel,
  line, tab-panel, list-view, tree-view
- Windows, macOS and Linux (GTK 3) only

## Build

Uses the [Siskin builder](https://github.com/Siskin-framework/Builder):

```sh
siskin Rebol-GUI.nest
```

The build generates `src/gen-gui.h` and `src/gen-gui.c` from `src/gui.reb` and
refreshes the reference sections of this file from the same specification. The
amalgamated `rebol-extension.h` of a matching Rebol3 build must be reachable by
the compiler.

On Linux the backend is built on GTK 3 (3.22 or newer). Install its
development files first - `libgtk-3-dev` on Debian and Ubuntu, `gtk3-devel` on
Fedora - and `pkg-config`, which the nest asks for the compiler and linker
flags.

## Usage

```rebol
gui: import 'gui

win: open-window/title 640x480 "Hello"
ok:  add-button win "OK" 20x20 100x32

do-events win func [evt][
    print [evt/type evt/offset]
    if all [evt/type = 'click  evt/source == ok] [print "clicked!"]
]
```

`do-events` runs until the window is closed. A loop of your own is
`poll-events` to take what is queued, then `wait` to sleep:

```rebol
forever [
    foreach evt poll-events [...]
    unless win/open? [break]
    wait [gui/event-port 0.05]
]
```

`poll-events` never sleeps; `wait` does, and the OS message queue is pumped
from inside it. Because the extension waits through Rebol's own `wait`, ports,
timers and other awake handlers keep working while a window is open, and
`gui/event-port` can be waited on together with anything else:

```rebol
wait [my-socket gui/event-port 1]
```

Waiting on the port returns as soon as there is something to poll; the number
is only the longest it will sleep. Events are not posted to
`system/ports/event` - `poll-events` is the only way to get them.

**Nothing is pumped while your handler runs.** There is one thread: a long
computation in a handler freezes the window for as long as it takes. Break long
jobs into pieces that return to the loop.

### Events

Each event is an `event!`:

| field    | type            | meaning                                              |
|----------|-----------------|------------------------------------------------------|
| `type`   | `word!`         | `move` `enter` `leave` `down` `up` `alt-down` `alt-up` `aux-down` `aux-up` `scroll-line` `close` `resize` `click` `change` `focus` `unfocus` `menu-select` `drop-file` `drop-text` `theme-change` |
| `source` | `handle!`       | the window, or the widget the event is about - for `move`, the one under the pointer; `evt/source/window` gets back to the window |
| `offset` | `pair!`         | the window's client coordinates, whatever the source; the new client size for `resize` |
| `flags`  | `block!`        | `shift` `control` `alt` `double`, where they apply   |
| `code`   | `integer!`/`word!` | signed wheel lines, or a menu item's word         |

The types are the core's own (`system/catalog/event-types`); the extension adds
no vocabulary. An event has an `offset` or a `code`, never both, so a wheel
event reports no position.

- `close` only *reports* that the user asked to close the window. It stays open
  until `close-window` is called, so a handler can refuse. (`do-events` closes
  it for you.)
- Enter in a `field` reports a `click`: the control was activated, not edited.
  An `area` keeps Enter for new lines.
- A `change` means the *user* changed something. Setting a value from Rebol
  does not report one.
- Consecutive `move` events from one source are collapsed to the newest.

### Mouse position

Every mouse event's `offset` is in the **client coordinates of its window** -
the window's own events, and a widget's alike. A `move` is reported wherever
the pointer is in the window, over widgets too, with the widget under the
pointer as `source` (or the window, over its background). Labels are
see-through: over one, the source is whatever holds it.

While a button is held, moves keep coming from wherever the press started,
even outside the window.

`enter` and `leave` say what the pointer is over, from the same moves: when a
`move` comes from a different source than the last one, the old source gets
`leave` and the new one `enter` - in that order, before the move. `enter` has
the position the pointer came in at, `leave` the last one it was seen at, each
in that source's own coordinates. Leaving the window altogether reports `leave`
too. During a press, nothing changes hands until the button is up.

```rebol
switch evt/type [
    enter [if evt/source == save-btn [show-tip "Saves the file"]]
    leave [if evt/source == save-btn [hide-tip]]
]
```

Outside this program's windows nothing is reported, unless asked for:

```rebol
track-mouse true     ;; returns whether it was on
```

Moves over the rest of the desktop then arrive with a **screen** handle as
`source`, and `offset` measured from that screen's top-left corner, so
`evt/offset + evt/source/offset` is in the space window offsets use. Over one of
this program's windows the window reports them as usual, never both. It is off
by default: with it on, any mouse movement anywhere on the desktop wakes `wait`.

To get a position relative to a widget, subtract its `at` - where its top-left
corner is in the window, however deeply it is nested. A window's `at` is
`0x0`, so this works whatever the source:

```rebol
evt/offset - evt/source/at    ;; the position within the source
evt/offset - canvas/at        ;; the position on a particular widget
```

`within?` tells whether a point is in a box - left and top edges in, right
and bottom out:

```rebol
if within? evt/offset canvas/at canvas/size [...]
```

**Comparing handles:** use `==` to ask "is this that widget?". `=` on two
handles compares their type only.

A closed window's handle stays valid and reports `open?` as `false`. Closing a
window also removes all its widgets: their handles report `id` 0 and `parent`
`none`, and setting anything on them fails. `remove-widget` does the same for
one widget (and, for a container, everything in it).

## Windows

```rebol
win: open-window/title 640x480 "Title"
open-window/fixed      400x300      ;; the user cannot resize it
open-window/borderless 400x300      ;; no title bar, no frame
open-window/hidden     400x300      ;; build it first, then show-window
```

`resizable?` and `title?` can be read and changed afterwards. Changing either
keeps the client size - the window grows or shrinks around it - and they are
independent: turning the title bar back on does not make a `/fixed` window
resizable.

`border?` says whether a window without a title bar still has a thin outline
and a shadow. A window opened with `/borderless` or `/transparent` starts
without one, and any other window starts with one. It reads `true` while
the window has a title bar, and the setting is kept when `title?` changes.
So hiding the title bar of a normal window leaves an outlined box, and
showing and hiding it again on a `/borderless` one leaves it bare, as it was:

```rebol
win/title?: false         ;; no title bar - outline and shadow stay
win/border?: false        ;; nothing around it at all
```

A **borderless window** has no title bar, so no close box and nothing to drag.
Give it your own way out, and move it yourself if needed:

```rebol
if type = 'down [grab: position]
if all [grab  type = 'move] [win/offset: win/offset + position - grab]
if type = 'up   [grab: none]
```

### Background

`background` is the client area's colour (`none` for the system colour), and
widgets on the window use it too:

```rebol
win/background: 24.26.34
lbl: add-text win "on the dark window" 10x10 0x0
lbl/transparent?: true
lbl/color: 225.228.235
```

`transparent?` makes the client area see-through, leaving only the widgets and
drawn pixels. Usually combined with `/borderless`:

```rebol
ghost: open-window/borderless/transparent 240x80
add-button ghost "floating" 20x20 0x0
```

On Windows this uses a colour key of pure magenta: anything drawn in exactly
`255.0.255` becomes a hole, and clicks there go to whatever is behind. On Linux
it needs a compositing window manager; without one the client area is black.

### Light and dark

`win/dark?` says whether the system shows the window in the dark appearance,
asked of the system each time. When the user switches, each window reports a
`theme-change` event with `light` or `dark` in `code` - once per switch:

```rebol
if evt/type = 'theme-change [
    win/background: either evt/code = 'dark [30.30.34][none]
]
```

On macOS and Linux the native controls and default colours follow the switch
by themselves. On Linux, `dark?` is read from the GTK theme - a theme whose
text is lighter than it is dark counts as dark - and switching themes reports
`theme-change`. On Windows only the title bar follows, unless the window asks
for more:

```rebol
win/dark-controls?: true
```

Then, while the system is dark, its buttons, toggles, checks, radios, fields,
areas, drop-lists, drop-downs, text-lists, sliders and progress bars are drawn dark, its
menu bar and menus turn dark, and every colour left at `none` (the window's
and panels' fill, text, the inside and the edges of fields) turns dark too. Off
by default, since it relies on undocumented parts of Windows. Colours the script set
are never changed - set them on `theme-change`.

### Painting

Creating widgets does not paint them one by one; everything added since the
last pump appears together, on the next `poll-events` or `wait`. A window
already on screen therefore fills in a moment after it is built. To have it
appear complete, open it `/hidden` and call `show-window` when done.

`redraw` repaints a widget after you have changed its image. On Windows it
paints before returning; on macOS and Linux it is drawn at the next pump, like
everything else there.

### Screens

`screens` returns one handle per connected display, the primary first, and
`win/screen` is the one holding most of a window:

```rebol
foreach scr screens [print [scr/name scr/size scr/scale]]

home: win/screen
home/offset home/size             ;; the whole display
home/work-offset home/work-size   ;; without the taskbar, the Dock or the menu bar
home/primary?

;; centre a window in the usable part of its screen
win/offset: home/work-offset + (home/work-size - win/size / 2)
```

Positions are in the same space as a window's `offset`, so the two can be
compared directly. Every read asks the system again, so a handle always
describes the display as it is now; one whose display was unplugged reads
`none` for everything. The same display is always the same handle, so use `==`
to ask whether two screens are the same one.

Each screen has its own `scale`. When screens have different scales there is
no single unit that covers the whole desktop, so each screen is measured in
its own: its `offset` is where it starts, and everything on it is in logical
units from there. Two screens can have a gap between them, but never overlap.
Within one screen - which is where a window is placed - everything matches the
window's own size.

On Windows, `name` is what the display settings show, which on some systems is
only "Generic PnP Monitor". On macOS before 10.15 a display is named by its id.
On Linux it is the connector (`DP-1`, `HDMI-2`), after the maker when the
system knows it.

On Linux, screens, a window's `offset` and `track-mouse` need X11: a Wayland
client may neither place its windows nor see the pointer outside them. The
extension therefore prefers X11, which on a Wayland desktop means XWayland;
`GDK_BACKEND=wayland` forces native Wayland, without those three.

## Widgets

Each `add-*` puts a native control into a window, a panel or an image widget,
and returns a handle. They take the container, a string, an offset and a size -
except `add-image`, which takes an `image!` instead of a string:

```rebol
btn:   add-button win "Click me"        20x20  140x32
label: add-text   win "Type your name:" 20x60  220x0   ;; 0 = work it out
name:  add-field  win ""                20x90  240x0
notes: add-area   win ""               20x130  300x200
opt:   add-check  win "Enabled"        20x340  200x24
one:   add-radio/group win "First"     20x370  110x22 1
pic:   add-image  win some-image       340x20
```

| kind        | control | reports |
|-------------|---------|---------|
| `button`    | push button | `down` `move` `up` `click` |
| `text`      | static label | nothing |
| `field`     | one-line entry | `change` `focus` `unfocus`, `click` on Enter |
| `area`      | multi-line entry with a scrollbar | `change` `focus` `unfocus` |
| `check`     | checkbox | `down` `move` `up` `click` |
| `radio`     | radio button | `down` `move` `up` `click` |
| `toggle`    | push button which stays pushed | `down` `move` `up` `click` |
| `slider`    | draggable slider | `down` `move` `up`, and `change` continuously while dragged |
| `progress`  | progress bar | nothing |
| `drop-list` | pick one of a list | `change` `focus` `unfocus` |
| `drop-down` | editable combo box - pick from a list, or type free text | `change` `focus` `unfocus` |
| `text-list` | pick one of a list shown in a box | `change` `focus` `unfocus` |
| `list-view` | a table - rows of cells under column headers | `change` `click` `sort` `focus` `unfocus` |
| `date-field` | a date, and optionally a time of day | `change` `click` `focus` `unfocus` |
| `panel`     | holds other widgets | nothing |
| `line`      | separator rule | nothing |
| `tab-panel` | tabs, a page (a panel) for each | `change` |
| `image`     | shows an `image!` | its own mouse events |

Buttons, checks, radios and sliders report the left mouse button going
`down` and `up` on them, and `move` from themselves while it is held -
wherever the pointer goes, not only over the control. Every `down` is followed
by exactly one `up`, and the order is `down`, then any `move` and `change`,
then `up`, then `click` (only when released over the control) - so a slider's
`down` and `up` bracket one drag:

```rebol
switch evt/type [
    down   [if evt/source == level [dragging: true]]
    change [if evt/source == level [preview level/value]]
    up     [if evt/source == level [dragging: false  commit level/value]]
]
```

Dragging with a control is ordinary arithmetic on window coordinates.
Moving a borderless window by one of its buttons - the client area moves with
the window, so the grab point stays put:

```rebol
switch evt/type [
    down [grab: evt/offset]
    move [if grab [win/offset: win/offset + evt/offset - grab]]
    up   [grab: none]
]
```

Moving the control itself inside the window - here the pointer moves
relative to the client area, so the grab point follows it:

```rebol
switch evt/type [
    down [grab: evt/offset]
    move [if grab [btn/offset: btn/offset + evt/offset - grab  grab: evt/offset]]
    up   [grab: none]
]
```

A `click` still follows a drag released over the control; ignore it if the
drag moved anything. These are mouse events only: pressing Space on a button, or moving a slider
with the arrow keys, reports `click` or `change` without them.

Common accessors; ones that do not apply to a kind answer `none`:

```rebol
btn/text: "Clicked"      ;; label, or the contents of a field or an area
btn/offset: 30x40        ;; position inside its container
btn/at                   ;; position in its window, however nested (read-only)
btn/size: 160x32
btn/enabled?: false
opt/state: true          ;; check, radio or toggle
bar/value: 40%           ;; slider or progress
btn/kind                 ;; button | text | field | ... | image
btn/parent               ;; the window or the container holding it
btn/window               ;; the window, however deeply nested
btn/tip: "Does this"     ;; tooltip; see below
win/children             ;; widgets held directly, in the order added
```

`children` answers `none` for a kind that cannot hold widgets, and a block
(possibly empty) for one that can. The block is the extension's own - read it,
but do not modify it.

### Tooltips

Any widget can have a tooltip, shown by the platform with its own delay,
placement and look:

```rebol
save-btn/tip: "Saves the file"
save-btn/tip: "Saves the file^/Ctrl+S"   ;; a newline starts a second line
save-btn/tip                           ;; what is set, or none
save-btn/tip: none                     ;; none, or "", takes it away
```

For anything the platform's tooltip cannot do - custom look, rich content,
status-bar hints - use `enter` and `leave` instead.

### Sizes and DPI

**Offsets and sizes are logical units** - 96 to the inch, a point on macOS, a
pixel at 100% scaling on Windows and Linux. The same layout is the same physical size on
any display, and event coordinates use the same units.

**A zero axis asks the widget what it needs**, from its font and its own
padding:

```rebol
add-field  win ""     300x100 240x0   ;; your width, its height
add-text   win "Name" 300x70  0x0     ;; fits the string
add-button win "OK"   20x20   0x0     ;; fits the label
```

This works on anything with text. A zero width on an entry gives room for about
twenty characters; a zero height on an `area` gives four lines. Image, slider,
progress and panel need an explicit size.

Measuring happens once, at creation. A widget never resizes itself later, so
after changing its font or text, assign a zero axis to ask again:

```rebol
lbl/font-size: 15
lbl/size: 220x0          ;; keep the width, measure the height
lbl/size: 0x0            ;; measure both
```

**`win/scale`** is device pixels per unit (`1.0`, `1.75`, `2.0` on Retina) of
the screen the window is on. Its main use is rendering an image at full
resolution:

```rebol
pic:    make image! to pair! 240x160 * win/scale
canvas: add-image/size win pic 20x70 240x160
```

Moving a window to a screen with another scale keeps its logical size, and
widget positions and text sizes with it, so the layout is unchanged. Only
`scale` changes: the window reports a `resize` (with the same size), which is
the moment to render its images again at the new scale.

On Windows this needs Windows 10 version 1703 or later. On older versions
every window uses the system scale, and Windows stretches a window shown on a
monitor with another setting. On Linux `scale` is GTK's whole-number scale
factor (`GDK_SCALE`); fractional scaling enlarges the text only.

### Text and colour

Anything with text - button, text, field, area, check, radio, drop-list, drop-down and a
panel's caption - takes typography:

```rebol
label/font:      "Georgia"   ;; none: the system font
label/font-size: 15          ;; points; none: the system size
label/bold?:     true
label/italic?:   false
label/color:     30.90.170   ;; none: the platform's colour
```

`font-size` is in points of 1/72 inch, so the same size is the same size on
every platform: 12 points is 16 logical units, on macOS too (where AppKit's own
"point" is the logical unit).

A window's `font` and `font-size` are defaults for widgets created **after**
they are set; widgets already on the window are not changed.

Not every platform honours every setting: a text colour on a Windows push
button is stored and read back but not shown. On macOS a text colour on a
drop-list (a pop-up button) is likewise stored but not shown; a drop-down's
(the combo box) colour does show, through `NSComboBox`'s own text colour.

`background` and `transparent?` work as on a window:

```rebol
lbl/background: 235.240.250   ;; fill with this
lbl/background: none          ;; the platform's own
lbl/transparent?: true        ;; nothing - what it sits on shows through
```

Setting a colour turns transparency off. A widget fills with the *window's*
colour by default, so a control inside a coloured panel or on an image needs
`transparent?: true` to blend in. Transparency applies to `text`, `check`,
`radio` and `panel`; entries, drop-lists and drop-downs take a `background` colour only.

### Fields and areas

```rebol
log/read-only?: true      ;; no typing, but selecting and copying still work
log/scroll: 'end          ;; or 'bottom, 'top, or a percent!
log/scroll                ;; where it is now, as a percent!
```

`read-only?` differs from `enabled?: false`, which also greys the text and
stops selection. Setting `text` may scroll an area back to the top, so a log
that should follow its newest line sets `scroll` after it:

```rebol
note: func [line [string!]][
    log/text: append append log/text newline line
    log/scroll: 'end
]
```

### Check boxes, toggles and radio groups

A toggle is a push button which stays pushed in until it is pushed again. It
is on or off like a check, and reads and writes the same way:

```rebol
bold: add-toggle win "Bold" 20x20 0x0
bold/state: true          ;; pushed in
if evt/source == bold [print ["bold is now" bold/state]]   ;; on `click`
```

Radios sharing a `/group` id turn each other off; anything with a different id
(no `/group` means 0) is left alone. Grouping follows the id only - not
creation order or which container they are in:

```rebol
warm: add-radio/group win "Warm"  20x290 110x22 1
cool: add-radio/group win "Cool"  20x315 110x22 1
slow: add-radio/group win "Slow" 150x290 110x22 2

cool/state: true    ;; turns `warm` off, leaves `slow` alone
one/group           ;; 1
```

Setting `state` from Rebol behaves exactly like a click, except that no event
is reported.

### Sliders and progress bars

Both take an offset and a size but no label, and carry a `value` from `0%` to
`100%`:

```rebol
level: add-slider/value   win 20x370 240x28 25%
meter: add-progress/value win 20x410 240x20 25%

level/value               ;; => 25%
meter/value: level/value  ;; decimal! is accepted too; out-of-range clamps
```

A slider taller than it is wide is vertical, with `0%` at the bottom.

### Drop-lists

```rebol
pick: add-drop-list/index win ["Ash" "Birch" "Elm"] 300x350 200x0 2

pick/items                ;; ["Ash" "Birch" "Elm"]
pick/index                ;; 2 - 1-based, 0 when nothing is picked
pick/text                 ;; "Birch" - read-only; set `index` to choose
pick/items: ["Oak" "Yew"] ;; replaces the list and clears the selection
```

Non-string values in the block are skipped. `size` is the closed control.
Duplicate entries are kept.

### Drop-downs

The editable counterpart of a drop-list: a combo box that lets the user pick
from a list *or* type free text of their own.

```rebol
combo: add-drop-down/index win ["Ash" "Birch" "Elm"] 300x350 200x0 2
combo/items                ;; ["Ash" "Birch" "Elm"]
combo/index                ;; 2
combo/text                 ;; "Birch" - can also be typed freely
combo/text: "Custom value" ;; sets the typed value directly
```

`add-drop-down` takes the same arguments as `add-drop-list` (parent, items,
offset, size, `/index n`). Unlike a drop-list, whose `text` is read-only,
a drop-down's `text` is both readable **and** writable - it is both what the
box shows and what the user may have typed - and typing in it reports
`change` just like editing a field.

### Text-lists

The same list, shown in a fixed box instead of behind a button. A vertical
scroll bar appears on its own while the items do not fit.

```rebol
trees: add-text-list/index win ["Ash" "Birch" "Elm" "Oak"] 300x425 200x80 2

trees/index               ;; 2 - 1-based, 0 when nothing is picked
trees/text                ;; "Birch" - none when nothing is picked; read-only
trees/index: 4            ;; picks and scrolls it into view, without a `change`
```

`items` and `index` work as for a drop-list. `change` is reported only when
the user picks. A zero size gives about twenty characters by six rows.

A field made with `/secure` masks what is typed, for a password, and refuses
to copy it out. `text` still reads and writes the real contents. `secure?`
reads it back. It is fixed at creation, because on macOS a masked field is a
different kind of control.

```rebol
pass: add-field/secure win "" 20x60 200x0
pass/secure?              ;; true
```

A field, an area and a text-list can be made without their border with
`/flat`. `border?` reads it back, and can turn it on or off at any time:

```rebol
plain: add-field/flat win "no border" 20x20 200x0
plain/border?             ;; false
plain/border?: true       ;; back to the platform's sunken edge
```

The box stays the same size either way, and the text gets the border's room.

`scroll` works as for an area. Setting `scrollable?` to false hides the
scroll bar and passes the wheel on to whatever holds the list. `scroll` and
`index` still move it, and so does picking with the keyboard:

```rebol
trees/scrollable?: false  ;; the user no longer scrolls it
trees/scroll: 'end        ;; code still does
trees/scroll: 3           ;; brings item 3 into view, without picking it
```

An integer `scroll` scrolls as little as it takes: not at all if the item is
already visible, and otherwise until it sits at the nearer edge. Values out
of range are clamped. Reading `scroll` still gives a percent.

### List-views

A table: rows of cells under column headers. It needs Rebol 3.22.10 or
later, for the `sort` event and for `RL_FORM_VALUE`.

```rebol
files: add-list-view/with/index win ["Name" 120  "Size" 60 right  "Date" none] 20x20 320x160 [
    "a.txt"   120  1-Jan-2026
    "b.png"  4096  2-Feb-2026
    "c.r3"     64  3-Mar-2026
] 2

files/index               ;; 2 - the row, 1-based; 0 when nothing is picked
files/text                ;; "b.png" - the first cell of that row, formed; read-only
files/items               ;; the block it was given - the very same one
files/columns             ;; ["Name" 120 "Size" 60 right "Date" none]
```

The columns are titles, each optionally followed by a width and an
alignment, in either order. A width of `none`, or no width at all, fits the
title. On the last shown column it fills the rest of the list instead, and
keeps filling it as the list or the other columns are resized.

A width of `0` hides a column. Its values stay in the rows, since a row is as
long as the whole spec, but nothing shows them. That suits an id or a path the
program needs and the user does not:

```rebol
files: add-list-view/with win ["id" 0  "Name" 140  "path" 0] 20x20 200x160 [
    101 "readme"  %docs/readme.txt
    102 "logo"    %img/logo.png
]
files/text                ;; the id of the picked row - the first value, hidden or not
```

`sort` reports a column by its position in the row, hidden columns counted, so
`sort/skip/compare files/items 3 event/code` sorts by the column clicked. The alignment is
`left` (the default), `center` or `right`, and applies to the title and the
cells:

```rebol
files/columns: ["Name" 140  "Size" 70 right  "Date" none center]
files/columns             ;; the same back - setting it again changes nothing
```

The cells are one flat block, row by row, as `foreach [name size date]` and
`sort/skip` want them. They can be any values, shown as `form` shows them.
`none` shows as an empty cell, and a cell is cut at 256 characters. A block
whose length is not a whole number of rows is refused.

The block is not copied. `items` gives back the same block, with its values
unchanged, and the cells are formed only when they are shown, so a long list
costs nothing until it is scrolled. Change the block in place and the rows
show the new values as they repaint. For a new number of rows, set it again:

```rebol
append files/items ["d.jpg" 512 4-Apr-2026]
files/items: files/items  ;; the new row shows up
```

Setting `columns` to a different number of columns clears `items`. When
every title is empty, `["" 140 "" none]`, there is no header row, and so no
`sort` either.

`background` takes two colours in a block for striped rows: the first, third
... rows get the first colour, the second, fourth... rows the second. Either
may be `none` for the platform's own, which decides whether the stripes start
on the first row or on the second. One colour, or `none`, takes the stripes
off again:

```rebol
files/background: [none 235.240.250]   ;; plain, blue, plain, blue...
files/background: [235.240.250 none]   ;; blue, plain, blue, plain...
files/background: none                 ;; plain again - on macOS too
```

The part below the last row gets the first colour when both are set, and the
platform's own otherwise.

The user picking a row reports `change`. A double click on a row, or Enter
with a row picked, reports `click`. Clicking a header reports `sort` with the
column number, 1-based, in `event/code`. Sorting is the script's job: sort
`items`, set it again, and set `sort-column` to show the arrow. A negative
column means descending:

```rebol
if event/type = 'sort [
    col: event/code
    dir: either col = files/sort-column [negate col][col]
    sort/skip/compare/reverse files/items 3 col          ;; or without /reverse
    files/items: files/items
    files/sort-column: dir
]
```

`index`, `scroll`, `border?`, `/flat`, fonts and `color` work as for a
text-list. `scrollable?` does not apply. A zero size gives the width of
the columns by six rows.

On Windows it uses the Explorer theme: whole-row selection and a highlight
under the pointer, as in the system's own file lists. With `dark-controls?`
on in a dark window it goes dark with the rest - rows, header and scroll
bars - and back again. Colours set with `color` and `background` are kept
either way.

### Tree-views

```rebol
tree: add-tree-view win [
    "Documents" docs [
        "Report.txt" report
        "Old" old ["a.txt" a  "b.txt"]
    ]
    "Music" music ["Song.mp3"]
    "Notes.txt"
] 20x20 220x300

tree/selected                  ;; docs/old/"b.txt" - the path to the picked node
tree/text                      ;; "b.txt" - its label
tree/selected: 'docs/report    ;; picks it, opening the branches above it
tree/selected: 'report         ;; the first node with that word...
tree/selected: "Song.mp3"      ;; ... or that label
tree/selected: none            ;; nothing picked
tree/expanded                  ;; [#(path! [docs]) docs/old] - the open branches
tree/expanded: [docs/old]      ;; opens exactly those, and the ones above
tree/nodes                     ;; every node's path, parents before children
tree/items: [...]              ;; replaces the whole tree
```

`items` is the menu bar's grammar without shortcuts: a label, then optionally
a word naming the node, then optionally a block of its children. A node is
found by its PATH - each node's word on the way down, or its label where it
has none - so words only have to be unique among siblings. A label cannot be
written in a path literal (`docs/"b.txt"` does not load), so such a path is
made with `to path! [docs "b.txt"]`; a block with the same values is taken
too. A one-node path molds as `#(path! [docs])`. Setting `selected` or
`expanded` to a path that names nothing picks, or opens, nothing; a single
word or label finds the first node with it, parents before children.

Events:

| `type` | when | `code` |
|---|---|---|
| `change` | the user picks another node | |
| `click` | a double click or Enter on a node | |
| `open`, `close` | the user opens or closes a branch | the node's position in `nodes` |

```rebol
if find [open close] evt/type [print [evt/type pick tree/nodes evt/code]]
```

Setting `selected`, `expanded` or `items` reports nothing, and neither does a
branch reopening because the one holding it did. A branch inside a closed one
keeps its state and shows it again when its parent opens, and `expanded`
lists it all the same. Fonts, `color`, `background`, `border?`, `/flat` and
`scroll` (a percent or `top`, `bottom`, `end`) work as for a text-list; a
zero size gives about eight rows.

Windows uses the TreeView control with the Explorer look, macOS an
`NSOutlineView`, Linux a `GtkTreeView`. On Windows a double click also opens
or closes a branch, as it does in Explorer. Right opens a branch, or moves
into an open one; Left closes it, or moves to the parent. macOS draws no
lines between the nodes; Windows and Linux do.

### Date-fields

```rebol
when: add-date-field/date/time win 20x20 0x0 24-Dec-2026/14:05

when/value                ;; 24-Dec-2026/14:05
when/value: 31-Dec-2026   ;; no time given - keeps the time of day
when/value: 1-Jan-2027/9:15
```

A new field shows the current date and time, and `/date` replaces them.
Without `/time` the field shows the date only, and `value` is a date
without a time. Values are local time, and a zone on a date you set is
ignored. `change` is reported only for the user's edits, and Enter is a
`click`, as in a field. On macOS the field keeps its own height and is
centred in a taller box.

Windows uses the Date and Time Picker, which drops down a calendar; with
`/time` it shows the user's short date and time formats. macOS uses
`NSDatePicker` as a field with a stepper. GTK 3 has no date picker, so on
Linux it is a field showing the user's short date (in ISO order where that
format has a two-digit year) with a calendar behind its icon; a typed date is
taken on Enter or when the field loses the focus, and one that does not read
as a date is put back. `dark-controls?` does not change
the Windows control yet. Requires Rebol 3.22.9, the first version that
passes a date's time to extensions.

### Panels and group boxes

A panel holds other widgets, positioned inside it. Panels nest:

```rebol
box:  add-panel win 20x285 260x60
warm: add-radio/group box "Warm" 10x26 110x22 1   ;; 10x26 within the box
```

`/border` draws a frame and `/title` a caption in it (a caption implies a frame).
Both can be changed later:

```rebol
box: add-panel/title win 20x285 260x60 "Temperature"
box/text: "Temperature (°C)"
box/border?: false
```

The frame is drawn inside the panel's box and never moves its children, so
leave room for it yourself.

A panel is not transparent to the mouse: clicks on its background are not
reported to the window. Removing a panel removes everything in it.

### Tab-panels

```rebol
tabs: add-tab-panel win ["General" "Advanced"] 20x20 300x200
general: first tabs/children          ;; a page per tab - a panel each
add-check general "Enabled" 10x10 0x0

tabs/index                ;; 1 - which tab is shown
tabs/index: 2             ;; shows the second page, without a `change`
tabs/text                 ;; "Advanced" - the label shown
```

The pages are made for you, one per label, and are in `children` in tab
order; each page's `group` is its tab. Put widgets on a page as on any
panel - not on the tab-panel itself. The size includes the tabs; a page
fills the rest. `change` is reported when the user picks a tab. `/index n`
picks the one shown first; an index out of range changes nothing, since one
page is always shown. `items` (the labels) and `text` are read-only for now,
and the tabs keep the platform's font and colours. On Windows the tabs do not
follow `dark-controls?` yet.

### Separator lines

```rebol
add-line win 20x280 300x0     ;; horizontal, 300 long
add-line box 140x10 0x40      ;; vertical, inside a panel
```

Wider than tall is horizontal. A zero axis is the line's own thickness; both
zero is an error. A line is decoration: it takes no focus, reports nothing
and, like a label, lets the mouse through to what holds it. Windows draws an
etched edge along the top (or left) of the box; macOS a hairline in the
system separator colour, which follows the dark appearance.

### Image widgets

`add-image` shows an `image!` and is where a renderer plugs in:

```rebol
pic:    make image! 240x160
canvas: add-image win pic 20x70                  ;; takes the image's size
canvas: add-image/size win pic 20x70 480x320     ;; ... or scales it

;; draw into `pic`, then:
redraw canvas
```

The widget holds the image itself, not a copy, so drawing into it and calling
`redraw` is all an animation needs. `canvas/image` returns that same image;
`canvas/image: other` swaps it (a different size is scaled into the widget's
box). Alpha is currently ignored.

An image widget **reports its own mouse events**, with itself as `source` -
which is what makes it a canvas. Like every mouse event they are in window
coordinates, so the point on the picture is `evt/offset - canvas/at`. A press
on the canvas keeps reporting moves from it until the button comes up, even
over a caption or outside the window.

It is also a container, so a caption can sit on the picture:

```rebol
cap: add-text canvas "frame 120" 8x8 200x0
cap/transparent?: true
cap/color: 255.255.255
```

## Menu bars

A menu is one block assigned to the window:

```rebol
win/menu: [
    "File" [
        "New"      new   #"N"          ;; Ctrl+N on Windows, Cmd+N on macOS
        "Save As…" save  [shift #"S"]  ;; with extra modifiers
        ---                            ;; a separator
        "Recent" [                     ;; a block after a label: a submenu
            "report.txt" recent-1
            "notes.txt"  recent-2
        ]
        ---
        "Close"    quit  #"W"
    ]
    "Help" ["About" about]
]

win/menu          ;; the block you assigned
win/menu: none    ;; removes the bar
```

| | |
|---|---|
| `---` | a separator |
| label + block | a submenu, in the same grammar |
| label + word | an item; the word is its id. An optional `char!`, or a block of modifier words and a `char!`, is its shortcut |

A pick is a `menu-select` event with the item's **word** in `code`, so renaming
a label never breaks a handler:

```rebol
if evt/type = 'menu-select [
    switch evt/code [
        new  [...]
        quit [close-window win]
    ]
]
```

Items are enabled and disabled by word; only the named ones change:

```rebol
win/menu-enabled?: [save false]
win/menu-enabled?    ;; [new true save false quit true about true]
```

Shortcuts always use the platform's menu modifier (Ctrl / Cmd); `shift`,
`control` and `alt` add to it.

Platform differences:

- On **Windows** the bar is inside the window. Adding or removing it keeps the
  client area the same size, so widgets never move.
- On **macOS** the bar is the application's, at the top of the screen, and
  shows the menu of whichever window is active. An application menu is added
  first; its Quit reports a `close` event for the window rather than ending
  the process.
- On **Linux** the bar is inside the window, as on Windows, and keeps the
  client area the same size.

### Context menus

`popup-menu` shows a menu and returns the word of the item picked, or `none`
when it is dismissed. The block is the menu bar's dialect:

```rebol
if all [evt/type = 'alt-down  evt/source == canvas] [
    switch popup-menu canvas [
        "Copy" copy  #"C"
        ---
        "More" ["Rotate" rotate  "Flip" flip]
    ][
        copy   [...]
        rotate [...]
    ]
]
```

It opens at the pointer, or with `/at` at an offset in the window's client
coordinates. The target is the window or any widget in it. It returns when
the menu closes, and reports no event; anything else that happens meanwhile
comes out of the next `poll-events`. Shortcuts are shown but do not work as
keys in a context menu.

## Modal dialogs

`open-window/modal owner` opens a dialog that blocks **every** other window of
the program until it closes. The owner only decides placement: the dialog is
centred on it (unless `/at` is given) and kept above it. The dialog has no
minimise button.

- Dialogs nest; only the newest takes input.
- `win/modal?` is read-only.
- Input events from blocked windows are dropped; `theme-change`, `resize` and
  `leave` still arrive.
- The user cannot close an owner while its dialog is open, and
  `close-window owner` errors with *The window has a modal dialog open!*.
- `release owner` closes its dialogs first (newest first).

```rebol
dlg: open-window/modal/title 260x100 win "Confirm"
do-events dlg :handler   ;; returns once the dialog is closed
```

## Keyboard focus

```rebol
set-focus name          ;; true; the window comes forward too
name/focused?           ;; true
set-focus label         ;; false - a label cannot take the focus
```

`set-focus` takes a widget, or a window. It returns `false` when the target
cannot take the focus: `text`, `image`, `panel`, `progress` and any disabled
widget.

Tab and Shift-Tab move between controls, including into panels and image
widgets; Space presses a focused button or check; `&` in a label marks a
mnemonic. On Windows the arrow keys also move within a radio group; on macOS
and Linux they do not. On macOS and Linux, Tab follows the controls' positions
rather than the order they were made in.

## Keys

```rebol
win/keys?: true           ;; off by default
```

With `keys?` on, every key pressed in the window is reported, whichever
widget has the focus. That widget is the event's `source`, or the window
when nothing in it is focused.

| `type` | `key` |
|---|---|
| `key`, `key-up` | a `char!`: `#"a"`, `#"A"`, `#"^M"` for Enter, `#"^-"` for Tab |
| `named-key`, `named-key-up` | a word from `system/catalog/event-keys`: `left`, `f1`, `escape`, `backspace`, `shift`... |

The character has Shift applied but not Control or Option/Alt: Ctrl+A is
`#"a"` with `control` in `flags`. On Windows and Linux, AltGr characters come
through as typed. A dead key reports nothing until it makes a character. Command on
macOS has no key and no flag, and is not reported. Held keys repeat.

Keys are only observed: the widget still gets them, and a handler cannot
stop that, because events reach Rebol after the platform has already handled
the key.

## Dropped files

Off until enabled per window:

```rebol
win/drop?: true
```

A drop arrives as `drop-file` (or `drop-text`), and its `source` is a handle
describing the drop:

```rebol
if evt/type = 'drop-file [
    print [evt/source/count "file(s) on" evt/source/target]
    foreach file evt/source/data [print file]
]
```

| field    | meaning                                                           |
|----------|-------------------------------------------------------------------|
| `kind`   | `files` or `text`                                                 |
| `data`   | a block of `file!`, or the `string!` for a text drop              |
| `count`  | how many - 1 for text                                             |
| `target` | the window, or the direct child of the window it landed on        |
| `window` | the window                                                        |

`offset` is where it landed, in the window's client coordinates like every
other event (`evt/offset - evt/source/target/at` for the target's own). A drop on a label sitting on an
image reports the image; walk `target/parent` if you need more.

## Platforms

| | Windows | macOS | Linux |
|---|---|---|---|
| file | `gui-win.c` | `gui-mac.m` | `gui-gtk.c` |
| built on | Win32 / GDI | AppKit | GTK 3 |
| needs | user32, gdi32, comctl32 | AppKit, Foundation, 10.13+ SDK | GTK 3.22+, X11 preferred |

Things worth knowing:

- **Coordinates** have a top-left origin on every platform, and `size` is
  always the client (content) area.
- **Mouse wheel** direction on macOS follows the user's "natural scrolling"
  setting.
- **Windows visual styles** need a Comctl32 v6 manifest in the `r3`
  executable; without one, controls use the classic look. With it, checks and
  radios animate between states.
- **macOS button height:** the rounded bezel is drawn for about 32 points; a
  taller button keeps it centred at that size.
- **macOS: never load two copies of the extension.** A standalone `gui.rebx`
  in a host that already embeds the same sources makes the Objective-C runtime
  report duplicated classes, and controls then misbehave. Builds with
  different `GUI_CLASS_PREFIX` values can coexist. The same holds on Linux,
  for GTK's type names.
- **Linux control sizes:** GTK's default theme sizes controls for touch as
  much as for a mouse. Inside this extension's windows, buttons, entries and
  tabs lose their minimum heights and some padding, so a layout sized for
  Windows or macOS fits.
## Extension commands:


#### `open-window` `:size`
Creates a window and returns its handle
* `size` `[pair!]` Size of the client area
* `/title`
* `text` `[string!]` Text shown in the title bar
* `/at`
* `offset` `[pair!]` Position of the top-left corner on the screen
* `/hidden` Creates the window without showing it
* `/fixed` The user cannot resize it
* `/borderless` No title bar and no frame - see the note in the README
* `/transparent` The client area is see-through to whatever is behind the window
* `/modal`
* `owner` `[handle!]` A dialog: every other window is blocked until it closes; centred on and kept above `owner`

#### `close-window` `:window`
Destroys the window
* `window` `[handle!]`

#### `show-window` `:window`
Makes the window visible
* `window` `[handle!]`

#### `hide-window` `:window`
Hides the window without destroying it
* `window` `[handle!]`

#### `poll-events`
Dispatches pending OS messages and returns the collected events

#### `add-button` `:parent` `:text` `:offset` `:size`
Creates a native push button and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]` Size of the button

#### `remove-widget` `:widget`
Destroys a widget
* `widget` `[handle!]`

#### `add-image` `:parent` `:image` `:offset`
Creates an image widget and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `image` `[image!]` Shown as is; the widget keeps a reference, not a copy
* `offset` `[pair!]` Position inside the parent
* `/size`
* `sz` `[pair!]` Scales the image to this size (default: the image's own)

#### `redraw` `:target`
Repaints a window or a widget - use after drawing into a displayed image
* `target` `[handle!]`

#### `add-text` `:parent` `:text` `:offset` `:size`
Creates a static label and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]`
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`

#### `add-field` `:parent` `:text` `:offset` `:size`
Creates a one-line text entry and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]` Initial contents
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/flat` Without the border - a plain box of text
* `/secure` Masks what is typed, for a password; copying out of it is refused

#### `add-area` `:parent` `:text` `:offset` `:size`
Creates a multi-line text entry and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]` Initial contents
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/flat` Without the border - a plain box of text

#### `add-check` `:parent` `:text` `:offset` `:size`
Creates a checkbox and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`

#### `add-radio` `:parent` `:text` `:offset` `:size`
Creates a radio button and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/group`
* `id` `[integer!]` Radios sharing an id turn each other off (default: 0)

#### `add-slider` `:parent` `:offset` `:size`
Creates a slider and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]` Taller than wide makes it vertical
* `/value`
* `val` `[percent! decimal!]` Initial position (default: 0%)

#### `add-progress` `:parent` `:offset` `:size`
Creates a progress bar and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/value`
* `val` `[percent! decimal!]` Initial position (default: 0%)

#### `add-drop-list` `:parent` `:items` `:offset` `:size`
Creates a drop-list - pick one of a list behind a button - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `items` `[block!]` Strings to offer
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]` Of the closed control; room for the list is added
* `/index`
* `n` `[integer!]` Item picked to start with, 1-based (default: none)

#### `add-panel` `:parent` `:offset` `:size`
Creates a panel - a widget which holds other widgets - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/border` Draws a frame around it
* `/title`
* `text` `[string!]` Caption set into the frame; implies /border

#### `gui-device`
Returns the id of the device this extension registered

#### `gui-device-polls`
Returns how many times the host has polled it

#### `gui-device-events`
Returns how many wake events the device has pushed

#### `gui-port-open` `:port`
Attaches a port to the GUI device
* `port` `[port!]`

#### `gui-port-close` `:port`
Detaches it again
* `port` `[port!]`

#### `gui-port-read` `:port`
Returns how many events are waiting
* `port` `[port!]`

#### `gui-device-pumps`
Returns how many polls reached the OS pump

#### `gui-device-messages`
Returns how many OS messages those pumps dispatched

#### `set-focus` `:target`
Gives a widget the keyboard focus; returns false if it cannot take it
* `target` `[handle!]` A widget, or a window to focus the window itself

#### `screens`
Returns a block of the connected screens, the primary one first

#### `track-mouse` `:on`
Reports `move` over the screens outside this program's windows, with the screen as the source; returns whether it was on
* `on` `[logic!]`

#### `add-toggle` `:parent` `:text` `:offset` `:size`
Creates a toggle - a push button which stays pushed - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`

#### `add-text-list` `:parent` `:items` `:offset` `:size`
Creates a list of strings in a fixed box, which scrolls when they do not fit, and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `items` `[block!]` Strings to show
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/index`
* `n` `[integer!]` Item picked to start with, 1-based (default: none)
* `/flat` Without the border

#### `add-date-field` `:parent` `:offset` `:size`
Creates an entry for a date, and optionally a time of day, and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/date`
* `when` `[date!]` Date (and time) to start with (default: now)
* `/time` Shows and edits the time of day as well

#### `add-drop-down` `:parent` `:items` `:offset` `:size`
Creates an editable combo box - a drop-down list which also takes typed text - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `items` `[block!]` Strings to offer
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]` Of the closed control; room for the list is added
* `/index`
* `n` `[integer!]` Item picked to start with, 1-based (default: none)

#### `add-line` `:parent` `:offset` `:size`
Creates a separator line - a static etched rule - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]` Wider than tall makes it horizontal; a zero axis is the line's own thickness

#### `add-tab-panel` `:parent` `:labels` `:offset` `:size`
Creates a tab-panel - a page per tab, only the picked one shown - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `labels` `[block!]` Tab labels; a page (a panel) is made for each, in `children`
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]` Of the whole control, tabs included
* `/index`
* `n` `[integer!]` Tab shown to start with, 1-based (default: 1)

#### `within?` `:point` `:offset` `:size`
Returns TRUE if the point is inside the box given by its offset and size
* `point` `[pair!]` Say an event's `offset`
* `offset` `[pair!]` Top-left corner of the box, say a widget's `at`
* `size` `[pair!]` Size of the box; the right and bottom edges are outside it

#### `popup-menu` `:target` `:items`
Shows a context menu and returns the word of the item picked, or none
* `target` `[handle!]` The window, or a widget in it
* `items` `[block!]` The same dialect as a window's `menu`
* `/at`
* `offset` `[pair!]` Where it opens, in the window's client coordinates (default: at the pointer)

#### `add-list-view` `:parent` `:columns` `:offset` `:size`
Creates a table - rows of cells under column headers - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `columns` `[block!]` Column titles, each optionally followed by its width (integer!, 0 to hide it, or none: fit the title, or fill the rest on the last shown column) and alignment (left, center or right)
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/with`
* `cells` `[block!]` Cells to show, row by row in one flat block; any values, shown as FORM shows them
* `/index`
* `n` `[integer!]` Row picked to start with, 1-based (default: none)
* `/flat` Without the border

#### `add-tree-view` `:parent` `:items` `:offset` `:size`
Creates a tree - nodes in branches which open and close - and returns its handle
* `parent` `[handle!]` Window, panel or image widget to put it in
* `items` `[block!]` Nodes: a label, then optionally a word naming it, then optionally a block of its children in the same grammar
* `offset` `[pair!]` Position inside the parent
* `size` `[pair!]`
* `/flat` Without the border


## Used handles and its getters / setters

#### __WINDOW__ - GUI window handle

```rebol
;Refinement       Gets                Sets                          Description
/title            string!             string!                       "Text shown in the title bar"
/size             pair!               pair!                         "Size of the client area, in logical units (see `scale`)"
/offset           pair!               pair!                         "Position of the top-left corner on the screen"
/at               pair!               none                          "Always 0x0 - a window's client area is where mouse offsets are measured from; here so that `evt/offset - evt/source/at` works for any source"
/id               integer!            none                          "Native window handle as an integer"
/open?            logic!              none                          "False once the window has been closed"
/scale            decimal!            none                          "Device pixels per unit of size - 1.0 at 100%, 1.75 at 175%, 2.0 on a Retina Mac"
/screen           handle!             none                          "The screen most of the window is on"
/dark?            logic!              none                          "Whether the system shows it in the dark appearance; a `theme-change` event reports when this changes"
/modal?           logic!              none                          "Whether it was opened with `/modal` - while it is open, every other window is blocked"
/dark-controls?   logic!              logic!                        "Whether its controls and default colours follow the dark appearance on Windows (macOS and Linux always do); off by default"
/keys?            logic!              logic!                        "Whether every key pressed in it is reported as `key`/`key-up` (a char!) or `named-key`/`named-key-up` (a word); off by default"
/resizable?       logic!              logic!                        "Whether the user can resize it"
/title?           logic!              logic!                        "Whether it has a title bar and a frame; without one the user cannot move or close it"
/border?          logic!              logic!                        "Whether it has a thin outline and a shadow; always true while it has a title bar, and remembered across `title?` changes"
/background       tuple!              [tuple! none!]                "Colour of the client area; none for the system window colour"
/transparent?     logic!              logic!                        "Whether the client area is see-through to whatever is behind the window"
/drop?            logic!              logic!                        "Whether files dropped on it are accepted; off until asked for"
/font             string!             [string! none!]               "Font family widgets are created with; none for the system font"
/font-size        integer!            [integer! none!]              "Point size widgets are created with; none for the system size"
/bold?            logic!              logic!                        "Whether widgets are created bold"
/italic?          logic!              logic!                        "Whether widgets are created italic"
/children         block!              none                          "Widgets the window holds directly, in the order they were added"
/menu             block!              [block! none!]                "The menu bar, as the dialect described in the README; none removes it"
/menu-enabled?    block!              block!                        "Which items are greyed out, as word/logic pairs; setting merges, it does not replace"
```

#### __DROP__ - GUI drop handle - what a drop-file or drop-text event carries

```rebol
;Refinement       Gets                Sets                          Description
/kind             word!               none                          "What was dropped: files or text"
/data             [block! string!]    none                          "A block of file! for a file drop, the string for a text drop"
/count            integer!            none                          "How many items - 1 for a text drop"
/target           handle!             none                          "The window or widget it was dropped on"
/window           handle!             none                          "The window it ended up in, however deeply nested"
```

#### __WIDGET__ - GUI widget handle - a native control inside a window

```rebol
;Refinement       Gets                Sets                          Description
/text             string!             string!                       "Label or contents; the caption of a framed panel; the selected item of a drop-list or a text-list, the first cell of a list-view's selected row, or the label of a tree-view's selected node, which are read-only; the typed value of a drop-down, which can be set; none for an image"
/items            block!              block!                        "Strings a drop-list, a drop-down or a text-list offers; a tab-panel's tab labels (read-only there); a list-view's cells, row by row in one flat block, any values; a tree-view's nodes, in the menu dialect's grammar; none for other kinds"
/index            integer!            integer!                      "Which item or list-view row is picked, or which tab is shown, 1-based; 0 for none"
/image            image!              image!                        "Image shown by an image widget, none for other kinds"
/size             pair!               pair!                         "Size of the control; a zero axis asks it what that axis needs, the same as at creation"
/offset           pair!               pair!                         "Position inside whatever holds it - a window or a panel"
/at               pair!               none                          "Top-left corner in its window's client area, however deeply nested - what a mouse event's offset is measured from"
/id               integer!            none                          "Native control handle as an integer"
/kind             word!               none                          "What the control is: button, image, text, field, area, check, radio, toggle, slider, progress, drop-list, drop-down, text-list, date-field, panel, line, tab-panel, list-view or tree-view"
/value            [percent! date!]    [percent! decimal! date!]     "Position of a slider or a progress bar; the date of a date-field, with its time of day when made with `/time`; none for other kinds"
/state            logic!              logic!                        "Whether a check, a radio or a toggle is on; none for other kinds"
/border?          logic!              logic!                        "Whether a panel draws a frame around itself, or a field, an area, a text-list, a list-view or a tree-view its border; none for other kinds"
/font             string!             [string! none!]               "Font family; none puts it back to the system font"
/font-size        integer!            [integer! none!]              "Point size; none puts it back to the system size"
/bold?            logic!              logic!                        "Whether the text is bold"
/italic?          logic!              logic!                        "Whether the text is italic"
/color            tuple!              [tuple! none!]                "Text colour; none lets the platform decide"
/background       [tuple! block!]     [tuple! block! none!]         "Colour painted behind the text; none lets the platform decide. A list-view also takes two colours in a block, which its rows alternate between - either may be none, for the platform's own"
/transparent?     logic!              logic!                        "Whether nothing is painted behind it at all, so whatever the widget sits on shows through"
/children         block!              none                          "Widgets a container holds, in the order they were added; none for a kind which cannot hold any"
/read-only?       logic!              logic!                        "Whether a field or an area refuses to be edited while staying selectable; none for other kinds"
/focused?         logic!              none                          "Whether it currently has the keyboard focus"
/scroll           percent!            [percent! decimal! word! integer!]"How far an area, a text-list, a list-view or a tree-view is scrolled; set a percent, or one of top, bottom and end; an integer brings that item or row into view; none for kinds which do not scroll"
/group            integer!            none                          "Which radio group it belongs to; for a tab-panel's page, which tab it is; 0 for everything else"
/enabled?         logic!              logic!                        "Whether the control responds to the user"
/tip              string!             [string! none!]               "Text the platform shows when the pointer rests on it; none for no tip"
/parent           handle!             none                          "Whatever holds it - a window, or a panel; none once gone"
/window           handle!             none                          "The window it ends up in, however deeply nested"
/scrollable?      logic!              logic!                        "Whether the user can scroll a text-list - off hides its scroll bar and ignores the wheel, while `scroll` and `index` still move it; none for other kinds"
/secure?          logic!              none                          "Whether a field masks what is typed - made with `/secure`; none for other kinds"
/columns          block!              block!                        "A list-view's columns: each a title, its width and, unless left, its alignment (center or right); a width of none fits the title, or fills the rest on the last shown column; 0 hides it, while its values stay in the rows; setting a different number of columns clears `items`; none for other kinds"
/sort-column      integer!            [integer! none!]              "Which list-view column shows the sort arrow, 1-based, negative for descending; none for no arrow. Only the arrow - sorting `items` is the script's"
/selected         path!               [path! word! string! none!]   "A tree-view's selected node, as the path to it: each node's word, or its label where it has none; setting a word or a label picks the first node that has it; none for no selection or for other kinds"
/expanded         block!              block!                        "A tree-view's open branches, a path for each; setting opens exactly those (with the branches above them) and closes the rest; none for other kinds"
/nodes            block!              none                          "A tree-view's nodes, a path for each, parents before their children - the `code` of an `open` or `close` event is a position in it; none for other kinds"
```

#### __SCREEN__ - GUI screen handle - one display; every read asks the platform again

```rebol
;Refinement       Gets                Sets                          Description
/name             string!             none                          "What the system calls the display"
/size             pair!               none                          "Size of the whole display"
/offset           pair!               none                          "Top-left corner, in the same space as a window's offset"
/work-size        pair!               none                          "Size of the part windows should use - without the taskbar, the Dock or the menu bar"
/work-offset      pair!               none                          "Top-left corner of that part"
/scale            decimal!            none                          "Device pixels per unit of size - what a window on this screen reports as its own scale"
/primary?         logic!              none                          "Whether this is the primary screen - the one with the menu bar on macOS"
```


## Other extension values:
```rebol
;; -----------------------------------------------------------------------
;; The event port - a doorbell, not a channel.
;;
;; The device pushes one Rebol event whenever the OS pump has put
;; something in the extension's queue, and an event has to be delivered
;; somewhere: EVM_PORT events resolve back to this port, its `awake` is
;; what puts it on WAIT's waked list, and the GC marks it for as long as
;; an event of its own is queued.
;;
;; The GUI events themselves never travel through it. They stay in the
;; extension's queue and come out of `poll-events`. All this port says is
;; "there is something to drain" - and `read` on it answers how much.
;;
;; It is open for the life of the module, which is what keeps the device's
;; one port slot pointing at something valid. A program writing its own
;; loop can wait on it alongside anything else:
;;
;;     wait [my-socket gui/event-port 1]
sys/make-scheme [
	title: "Rebol/GUI event doorbell"
	name:  'gui
	actor: object [
		open:  func [port] [gui-port-open  port]
		close: func [port] [gui-port-close port]
		read:  func [port] [gui-port-read  port]
	]
]

event-port: try [open [scheme: 'gui]]

;; Returning TRUE is what wakes WAIT. Without an awake handler the system
;; port takes the event off its queue, finds nothing to call, and drops
;; it - the doorbell would ring into an empty hall and every wait would
;; sleep out its full timeout.
if port? event-port [event-port/awake: func [event] [true]]

;; `poll-events` returns a block of event! values, so a handler takes
;; ONE argument and reads what it needs by name:
;;
;;     foreach evt poll-events [
;;         switch evt/type [
;;             click  [print [evt/source "at" evt/offset]]
;;             change [...]
;;         ]
;;     ]
;;
;; `source` is the window for window events and the WIDGET itself for a
;; click, a change and a focus change - use `evt/source/window` to get
;; back to the window it is in.
;;
;; The type words are the core's own: this extension defines no event
;; vocabulary of its own, so they are the ones in
;; system/catalog/event-types that every other event source reports.
do-events: function [
	"Pumps window events until the given window is closed"
	window  [handle!]
	handler [any-function!] "Called with one event! for each event"
	/rate delay [number!] {Longest it may sleep with nothing to do (default: 0.05)}
][
	;; `wait` does the sleeping, and that is the whole point: the
	;; extension registers a device with RDO_AUTO_POLL, so the host pumps
	;; the OS message queue from inside OS_Wait - and everything else
	;; Rebol has waiting is serviced by the same sleep.
	;;
	;; Waiting on the event port as well as the delay is what makes this
	;; responsive rather than merely correct: the device pushes an event
	;; when the queue grows, so `wait` returns as soon as a click arrives
	;; and the delay is only a ceiling. Without the port it still works,
	;; just at the rate of the ceiling.
	delay: any [delay 0.05]
	wake:  either port? event-port [reduce [event-port delay]][delay]

	forever [
		foreach evt poll-events [
			handler evt
			;; `close` only reports the request - closing is ours to do
			if all [evt/type = 'close  evt/source = window] [
				close-window window
			]
			;; the handler is allowed to close it as well, and the rest
			;; of this batch would then refer to widgets which are gone
			unless window/open? [break]
		]
		unless window/open? [exit]
		wait wake
	]
]
```
