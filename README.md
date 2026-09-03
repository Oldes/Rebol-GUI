# Rebol/GUI extension

A minimal windowing extension for [Rebol3](https://github.com/Oldes/Rebol3),
built on the current extension ABI.

This is a deliberate restart of the old `host-window.c` / `host-event.c` /
`host-compositor.c` / `host-draw.c` sources. It does not use `gob!`, it does
not composite, and it does not evaluate the DRAW dialect. It opens windows and
reports what the mouse did in them - and it does so without a single change to
the interpreter.

Drawing arrives through the image widget: build an `image!`, render into it
with whatever draws pixels ([Blend2D](https://github.com/Siskin-framework/Rebol-Blend2D)
being the intended one), and `redraw` it.

## What it is not (yet)

- no DRAW dialect, no compositor - just the image widget described below
- no keyboard events and no menus
- eleven native controls so far: button, image, text, field, area, check,
  radio, slider, progress, drop-down, panel
- no `system/ports/event` integration - see below
- Windows and macOS; there is no X11/Wayland backend yet

## Why `poll-events` and not the event port

Events could be posted with `RL_Event`, but nothing would pump the OS message
queue while the interpreter sits in `wait`: an extension cannot register a
device, and `RDI_EVENT` is a fixed slot in the host's device table. So the
extension keeps its own queue and `poll-events` both pumps and drains it.

The cost is an explicit loop. The benefit is that this builds and runs against
an unmodified `r3`, with no host-lib callbacks and no `REBGOB` crossing the
extension boundary - and the loop is one mezzanine function (`do-events`) which
can be replaced the day the interpreter grows a poll hook.

## Build

Uses the [Siskin builder](https://github.com/Siskin-framework/Builder):

```sh
siskin Rebol-GUI.nest
```

The build generates `src/gen-gui.h` and `src/gen-gui.c` from `src/gui.reb`, and
refreshes the reference sections of this file from the same specification. The
amalgamated `rebol-extension.h` of a matching Rebol3 build must be reachable by
the compiler.

## Usage

```rebol
gui: import 'gui

win: open-window/title 640x480 "Hello"
ok:  add-button win "OK" 20x20 100x32

forever [
    foreach [type source position value] poll-events [
        print [type position value]
        if all [type = 'click  source/id = ok/id] [print "clicked!"]
        if type = 'close [close-window win  halt]
    ]
    wait 0.01
]
```

Every event is four values, so a `foreach` takes them apart directly:

| value      | type       | meaning                                              |
|------------|------------|------------------------------------------------------|
| `type`     | `word!`    | `move` `down` `up` `alt-down` `alt-up` `aux-down` `aux-up` `wheel` `close` `resize` `click` `change` `focus` `unfocus` |
| `source`   | `handle!`  | what produced it: the window, or the widget for `click` |
| `position` | `pair!`    | client coordinates; the new client size for `resize`, the widget's offset for `click` |
| `value`    | `integer!` | modifier bits, or signed wheel lines                  |

The modifier bits are named by the exported `event-flags` object
(`shift` 1, `control` 2, `alt` 4, `double` 8).

Two notes on behaviour:

- `close` only *reports* that the user asked to close the window. The window
  stays open until Rebol calls `close-window`, so a handler can refuse.
- consecutive `move` events for one window are collapsed to the newest one,
  which keeps a fast mouse from filling the queue.

A closed window's handle stays valid and reports `open?` as `false`; it never
becomes a dangling pointer.

## Widgets

Each `add-*` puts a native control into a window's client area and returns a
handle of its own. They all take the same arguments - window, string, offset,
size - except `add-image`, which takes an `image!` instead of a string:

```rebol
btn:   add-button win "Click me"       20x20  140x32
label: add-text   win "Type your name:" 20x60 220x24
name:  add-field  win ""                20x90 240x26
notes: add-area   win ""               20x130 300x200
opt:   add-check  win "Enabled"        20x340 200x24
one:   add-radio/group win "First"     20x370 110x22 1
pic:   add-image  win some-image       340x20
```

| kind     | control | reports |
|----------|---------|---------|
| `button` | push button | `click` |
| `text`   | static label | nothing |
| `field`  | one-line entry | `change` `focus` `unfocus` |
| `area`   | multi-line entry with a scrollbar | `change` `focus` `unfocus` |
| `check`  | checkbox | `click` |
| `radio`  | radio button, see grouping below | `click` |
| `slider` | draggable slider | `change`, continuously while dragged |
| `progress` | progress bar | nothing; it takes no input |
| `drop-down` | pick one of a list | `change` `focus` `unfocus` |
| `panel`  | holds other widgets, see below | nothing |
| `image`  | an `image!`, see below | its own mouse events |

Every accessor works on every kind that has one:

```rebol
btn/text: "Clicked"      ;; label, or the contents of a field or an area
name/text                ;; what the user has typed
btn/offset: 30x40        ;; position inside the client area
btn/size: 160x32
btn/enabled?: false      ;; everything except an image
opt/state: true          ;; a check or a radio; none for the rest
bar/value: 40%           ;; a slider or a progress bar; none for the rest
pick/items: ["a" "b"]    ;; a drop-down; none for the rest
pick/index: 2            ;; which item, 1-based; 0 for none
one/group                ;; which radio group, 0 for everything else
btn/kind                 ;; button | text | field | area | check | radio |
                         ;; slider | progress | drop-down | panel | image
btn/parent               ;; whatever holds it: a window, or a panel
btn/window               ;; the window either way, however deeply nested
```

**`change` means the user changed it.** Writing `field/text: "x"` from Rebol
does not raise one - on Windows that takes suppressing the `EN_CHANGE` which
`SetWindowText` sends synchronously, while on macOS `setStringValue:` simply
never calls the delegate. Same behaviour, two different reasons.

Clicking one queues a `click` event whose `source` is the widget. `source/id`
is what identifies it - handles are compared by the native handle behind them,
not by `=`.

Accessors that belong to one kind answer `none` on the others - `btn/image`
and `pic/text` are both `none`, and `widget/kind` says which is which.

An `area` is one control in Rebol's eyes but two objects on macOS: the handle
is the `NSScrollView`, and the `NSTextView` inside it holds the text and the
back-pointer. Everything above the backend is unaware of that.

### Typography

Anything that has `text` has typography — button, label, field, area, check,
radio, drop-down and a panel's caption. Anything that doesn't (image, slider,
progress) answers `none`:

```rebol
label/font:      "Georgia"   ;; family name; none goes back to the system font
label/font-size: 15          ;; points;      none goes back to the system size
label/bold?:     true
label/italic?:   false
label/color:     30.90.170   ;; text colour; none lets the platform decide
```

**A widget's font is read back out of the control, not out of a note this
extension kept.** So what you read is what is on screen — including the font a
control was born with, and one set by anything else. There is nothing to drift
out of step, at the price of a font being one indivisible object underneath:
setting `font-size` alone means reading the font, changing the size and putting
it back, which the shared layer does for you.

The colour is the exception, and is kept per widget. Win32 stores no text
colour on a control — the *parent* is asked, message by message, as each child
is about to paint — so there is nowhere in the control to read one back from.

**A control does not resize itself for a bigger font.** The box laid out is the
box kept, exactly as with a panel's frame, so leave room.

#### A window's default

A window carries a default that widgets pick up **as they are created**:

```rebol
win/font: "Georgia"
win/font-size: 16
add-button win "Big" 20x20 100x30      ;; Georgia 16

win/font-size: 11
add-button win "Small" 20x60 100x30    ;; Georgia 11 - the first is still 16
```

Setting it reaches back into nothing. That is the whole of the inheritance:
the default is read once, at creation, so a widget's font stays its own
business and there is no "inherited or explicit?" bit to keep straight. To
restyle what is already on screen, loop over the handles.

There is deliberately no `win/color`. A window has no text of its own, and a
colour on a window would read as a background colour — which this extension
does not paint.

#### What each platform will not do

| | |
|---|---|
| Windows push button | ignores a text colour. Win32 draws a `BS_PUSHBUTTON`'s text itself, in the system colour; only an owner-drawn button could say otherwise, and owner-drawing means giving up the native look for every button. The colour is still stored and still reads back — it just does not show. A coloured label above the button is the usual way round it. |
| macOS drop-down | ignores a text colour. A pop-up button shows whichever menu item is selected rather than a title of its own, so an attributed title would fight the menu. |

Fonts and sizes work everywhere, on both platforms, including on those two.

Under the hood the two backends have almost nothing in common here. Windows
caches an `HFONT` per distinct face and deletes them all at shutdown — a GDI
object belongs to whoever made it, and a control does not own the font it is
given, so restyling one label in a loop creates one font rather than one per
assignment. macOS asks `NSFontManager` for a family and converts it for bold
or italic, and colours a button by rebuilding its **attributed title** — which
is why setting a button's text or font rebuilds that string too.

### Panels

A panel holds other widgets. Every `add-*` takes a window **or a panel** as its
first argument, and what a panel holds is positioned inside *it*:

```rebol
box:  add-panel win 20x285 260x60
warm: add-radio/group box "Warm" 10x26 110x22 1   ;; 10x26 within the box
```

`parent` is whatever holds a widget; `window` is the window either way,
however deeply nested. Panels nest inside panels.

**A widget still belongs to its window, not to its panel.** The window keeps
one flat list of every widget at any depth, which is what keeps closing a
window a single walk — the nesting only says where a control sits.

`remove-widget` on a panel takes its contents with it, and their handles turn
into removed ones in the same moment. The children are destroyed *first*,
depth-first, rather than being left to the container: Windows would take them
anyway, but on macOS this extension holds a reference to every control, so
letting the panel drop them would leak one object each.

**No view in the macOS backend fills a background.** A panel with nothing to
say, and an image widget with no pixels yet, both used to paint their bounds
with the window background colour — the ordinary thing to do — and both took
every widget created before them off the window. Whatever the mechanism (their
frames were verified correct), an opaque fill in a view that has nothing to
show is how a missing image turns into missing buttons. A view that draws
nothing hides nothing, so that is what they do.

A panel draws nothing of its own unless asked. It is a place to put things,
not a visible slab — and on macOS an opaque container is what turns any
mistake about its frame into a blank window instead of a misplaced rectangle,
so it paints no background at all. On Windows a child window has to paint or
it shows whatever was behind it, and Win32 clips a child strictly to its own
rectangle, so there it fills with the window background colour.

#### Group boxes

`/edge` gives a panel a frame, and `/title` puts a caption in that frame — a
group box. A caption implies the frame, because a group box without one is
just floating text:

```rebol
box: add-panel/title win 20x285 260x60 "Temperature"
add-radio/group box "Warm" 10x26 110x22 1

box/edge                    ;; true
box/text: "Temperature (°C)"    ;; retitles it
box/edge: false             ;; and it is a plain container again
```

Both are read at paint time rather than baked into the control, so
`box/edge: true` on a panel that has been on screen for an hour is a repaint,
not a rebuild.

**The frame never moves anything.** It is drawn inside the panel's own box,
and a child is positioned from the panel's top-left corner whether there is a
frame or not — so adding an edge to an existing panel cannot shift its
contents, and leaving room for the frame and the caption is the caller's job.
That is also why the frame is drawn by hand on both platforms rather than
handed to the native control for it:

| | native option | why not |
|---|---|---|
| Windows | `BUTTON` with `BS_GROUPBOX` | not a container — a group box is a sibling drawn *behind* the controls it appears to hold, and parenting children to one is where the classic repaint and tab-order trouble starts |
| macOS | `NSBox` | puts its children in a `contentView` of its own and insets them by an amount it chooses — so a panel would move its contents the moment it grew an edge, and by a different amount than Windows |

Each backend measures the caption with its own font, so the two frames are not
identical to the pixel. They are not allowed to matter: nothing is laid out
from them.

On macOS the top line is drawn in two pieces with a gap for the caption,
rather than the usual trick of painting the background back over the text.
Breaking the line needs no colour and so cannot cover anything — which, given
what an opaque fill in a container did to this backend once already, is worth
the four extra lines. On Windows the panel owns its background colour (it
filled the client area with it a moment earlier), so there the gap is painted
back exactly rather than guessed at.

Two things to know:

- **A panel is not transparent to the mouse.** It is a real child window /
  view, so mouse events over its background are not reported to the window.
  Only the image widget reports events for its own area.
- **Windows notifications go to the parent**, which for a control inside a
  panel is the panel — not the window, where all the reporting lives. The
  panel's window procedure therefore forwards `WM_COMMAND`, the scroll
  messages and the control-colour messages straight up. That works because
  the handlers identify a control from `lParam` rather than from whichever
  window received the message.

### Drop-downs

The one control that takes a list:

```rebol
pick: add-drop-down/index win ["Ash" "Birch" "Elm"] 300x350 200x26 2

pick/items                ;; => ["Ash" "Birch" "Elm"]
pick/index                ;; => 2, 1-based; 0 when nothing is picked
pick/text                 ;; => "Birch" - read-only, `index` is what chooses
pick/items: ["Oak" "Yew"] ;; replaces the list, dropping the old selection
```

Non-string values in the block are skipped rather than refused: a block of
words or files is a reasonable thing to hand over, and `form`-ing it first is
the caller's business.

The block↔list marshalling lives in the shared layer; a backend only knows how
to count, read, add and clear one item at a time (`Gui_Widget_Get_Item` and
friends). That is the same split the image widget uses, and it keeps the same
Rebol-side code running on both platforms.

Two platform notes:

- **`size` is the closed control.** On Windows the height passed to
  `CreateWindow` for a `COMBOBOX` sets the height of the *whole* thing,
  dropped list included — ask for 26 and the list is a sliver. The backend
  adds room for the list and reports the closed height back from
  `CB_GETITEMHEIGHT`, so what you set is what you read. Cocoa needs none of
  this; the popup's list is drawn outside its frame.
- **Duplicate entries survive.** `NSPopUpButton`'s convenience methods treat
  titles as a menu's identity and would drop a repeat, so items are added as
  `NSMenuItem`s directly. A list of strings here is data, not a menu.

### Sliders and progress bars

Both take an offset and a size but no label, and both carry a `value` from
`0%` to `100%`:

```rebol
level: add-slider/value   win 20x370 240x28 25%
meter: add-progress/value win 20x410 240x20 25%

level/value               ;; => 25%  (a percent!, not a raw decimal)
meter/value: level/value  ;; decimal! is accepted too; out-of-range clamps
```

A slider **taller than it is wide is vertical** — the same rule the old View
widgets used, and one argument fewer to pass. `0%` is always the bottom or the
left end: Win32 puts position 0 at the *top* of a vertical trackbar, so the
Windows backend flips both directions and the caller never sees it.

Underneath, the value is a fraction. Windows works in whole steps, so it is
carried as one part in 1000; macOS gives the control a 0.0–1.0 range directly.
Either way the resolution is finer than the pixels involved.

### Radio groups

Radios that share a `/group` id turn each other off. Anything with a different
id — or no `/group` at all, which means group 0 — is left alone:

```rebol
warm: add-radio/group win "Warm"  20x290 110x22 1
cool: add-radio/group win "Cool"  20x315 110x22 1
slow: add-radio/group win "Slow" 150x290 110x22 2

cool/state: true    ;; turns `warm` off, leaves `slow` alone
```

**The grouping is this extension's, not the platform's** — and that is a
deliberate refusal of both native behaviours, because neither means what the
caller does. Win32 groups radios by sibling order bounded by `WS_GROUP` flags;
AppKit groups them by shared superview *and* action selector. Every control
here is a direct child of the window and every button shares one action, so
both would sweep every radio in a window into a single group, and the answer
would depend on the order things were created in.

Instead the widget's `state` is the truth, the window's widget list is walked
to settle a group, and the native controls are then made to agree. Setting
`state` from Rebol goes through exactly the same path as a click, so both
behave alike.

On Windows the control is created `BS_RADIOBUTTON` rather than
`BS_AUTORADIOBUTTON`, so it reports the click without deciding anything.

macOS takes one more step. AppKit clears radio siblings when one is clicked,
so every radio in the window — not just the group — has to be written back
afterwards. The catch is that a programmatic `setState:` counts as a group
operation too, so writing the others back re-triggers the clearing, and only
the radio written last survives. The backend therefore detaches the button's
action for the length of each state write, which takes it out of any group
AppKit can see; the writes then mean what they say.

A checkbox needs none of this: it toggles itself and the new state is simply
read back before the `click` event goes out.

Widget handles are independent of their window's handle: keep one, drop the
other, release them in any order. **A widget dies with its window.** Closing a
window destroys every control the OS gave it, so all of that window's widget
handles turn into removed ones in the same moment - their `id` becomes 0,
`parent` becomes `none`, and setting anything on them fails rather than
writing through a freed pointer. `remove-widget` does the same for one widget
on its own.

### Image widgets

`add-image` shows a Rebol `image!` in a window, and is the seam a renderer
plugs into:

```rebol
pic:    make image! 240x160
canvas: add-image win pic 20x70          ;; widget takes the image's size
canvas: add-image/size win pic 20x70 480x320   ;; ... or scales it

;; draw into that very image - with a renderer, or by hand - then:
redraw canvas
```

**The widget holds a reference, not a copy.** The `image!` series is kept in
the handle context's `series` field, which the GC marks, so the image stays
alive as long as a widget shows it. Both backends read the dimensions and the
pixel pointer *fresh at every paint*, never caching either - a series can move
when it is expanded, and its size can change under the widget.

That is what makes the draw-then-`redraw` loop cost nothing: no copy, no
re-registration, no conversion. `image!` is BGRA on both platforms, which is
exactly a 32-bit `BI_RGB` DIB on Windows and
`kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little` on macOS, so the
pixels reach the screen untouched. Alpha is currently ignored - the image is
blitted opaque.

Assigning `canvas/image: other-pic` swaps the reference and repaints; the
widget keeps its own box, and a differently sized image is scaled into it.

Reading `canvas/image` hands back that same series - not a copy - so it is
also how a renderer gets at the pixels it is meant to fill.

One ABI note for anyone porting this. `RXIARG` is a union, and the image and
series views of it overlap:

```c
struct { void *series; u32 index; };
struct { void *image;  int width:16; int height:16; };
```

`index` and the two dimension bitfields are *the same four bytes*, so an
`RXIARG` carrying a 240x160 image unavoidably reads as `index` 0x00A000F0.
The accessor fills in the image view and nothing else - `index` is not a
thing an `image!` has, since an image takes its size from its own series -
and the conversion on the other side ignores it, in both directions. An
interpreter which instead reads that index back on the handle path will walk
ten million values off the end of the series, so this wants a core new enough
to ignore it.

An image widget also **reports its own mouse events**: a child control covers
its part of the window, so the window stops hearing about the mouse there. The
events arrive with the widget as `source` and coordinates relative to it,
which is what makes the widget usable as a canvas rather than a picture.

Under the hood a window keeps an intrusive list of its widgets. It is the only
reason the extension can tell the Rebol handles that the OS took their
controls away.

## Platforms

Everything above the OS is shared. A backend implements the dozen `Gui_*`
functions declared in `gui.h` and nothing else - no command, no accessor and
no part of the event queue is platform aware.

| | Windows | macOS |
|---|---|---|
| file | `gui-win.c` | `gui-mac.m` |
| built on | Win32 / GDI | AppKit |
| needs | user32, gdi32, comctl32 | AppKit, Foundation, a 10.13+ SDK |

Coordinates are uniform: positions and sizes use a top-left origin with Y
growing down, and `size` always means the *client* (content) area. The Cocoa
backend flips every offset against the menu-bar screen on the way in and out,
and its content view answers `YES` to `isFlipped` so that event positions need
no conversion at all.

Differences worth knowing about:

- **Retina.** macOS sizes are in points, not pixels: a 640x480 window has a
  1280x960 backing store. An image widget is scaled into its box in points, so
  on a Retina display a 240x160 image is drawn across 480x320 device pixels
  and looks soft. Asking for an image twice the size and a box of the intended
  point size is the workaround until the backing scale factor is plumbed
  through.
- **Main thread.** AppKit requires every call to be made from the main thread,
  which is where the interpreter runs. `Gui_Init_Platform` also calls
  `finishLaunching` and sets a regular activation policy, without which a
  non-bundled `r3` opens windows that never come forward.
- **No menu bar** is installed on macOS. A Quit item would terminate the
  process behind Rebol's back, which is the very thing `close` events exist to
  avoid.
- **Wheel direction** follows the user's "natural scrolling" setting, like
  every other Mac application; the sign is not normalised.
- **Dragging.** Cocoa reports movement with a button held as a drag rather
  than a move; those are reported as plain `move` events, matching Windows,
  which captures the mouse instead.
- **All macOS drawing happens in the pump.** `NSApplication`'s own `-run`
  loop displays dirty views between events, and this extension never calls
  `-run` — so `Gui_Pump` does it, and nothing else does. Creating a widget or
  calling `redraw` only *marks* it; it appears at the next `poll-events`.
  Forcing a display at any other moment is unreliable: a display asked for
  while AppKit is not ready can clear a view's needs-display flag without
  painting, after which nothing marks it again and the control stays blank.
  A batch of widgets created without an intervening poll is exactly the case
  that breaks, so a program which never polls will see an empty window —
  which is true of any AppKit program that does not run its loop. Windows has
  no such rule: `UpdateWindow` paints on the spot.
- **Never load two copies at once (macOS).** Objective-C class names live in
  one process-wide namespace, not per binary. A standalone `.rebx` loaded into
  a host that has these same sources embedded gives:

  ```
  objc: Class RebolGuiButton is implemented in both …/rebol3 and …/gui.rebx.
        This may cause spurious casting failures and mysterious crashes.
  ```

  The runtime keeps one implementation of each duplicated class and messages
  from the other binary land in it — so some controls half work, or never
  draw, and nothing in the source looks wrong. The class names therefore carry
  a build-set prefix (`GUI_CLASS_PREFIX`; the nest gives the standalone build
  `RebolGuiRebx`, embedded builds get the default). Two builds with *different*
  prefixes can coexist; two with the same prefix cannot. Windows has no
  equivalent problem — window classes are registered per module instance.
- **Keyboard.** There are no key events yet, and no way to move focus from
  Rebol - a field is reached with the mouse or with Tab.
- **Button look.** On Windows the control uses the shell's message font, but
  visual styles need an application manifest naming Comctl32 v6 - which
  belongs to the `r3` executable, not to a DLL it loads. Without one, buttons
  come out in the flat pre-XP style. On macOS the rounded bezel is drawn for
  a fixed height of about 32 points; a taller button keeps its bezel that
  size and centres it, so `32` is the height to ask for.

## Extension commands:


#### `open-window` `:size`
Creates a window and returns its handle
* `size` `[pair!]` Size of the client area
* `/title`
* `text` `[string!]` Text shown in the title bar
* `/at`
* `offset` `[pair!]` Position of the top-left corner on the screen
* `/hidden` Creates the window without showing it

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
Creates a native push button inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]` Size of the button

#### `remove-widget` `:widget`
Destroys a widget
* `widget` `[handle!]`

#### `add-image` `:parent` `:image` `:offset`
Creates an image widget inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `image` `[image!]` Shown as is; the widget keeps a reference, not a copy
* `offset` `[pair!]` Position inside the client area
* `/size`
* `sz` `[pair!]` Scales the image to this size (default: the image's own)

#### `redraw` `:target`
Repaints a window or a widget - use after drawing into a displayed image
* `target` `[handle!]`

#### `add-text` `:parent` `:text` `:offset` `:size`
Creates a static label inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `text` `[string!]`
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`

#### `add-field` `:parent` `:text` `:offset` `:size`
Creates a one-line text entry inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `text` `[string!]` Initial contents
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`

#### `add-area` `:parent` `:text` `:offset` `:size`
Creates a multi-line text entry inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `text` `[string!]` Initial contents
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`

#### `add-check` `:parent` `:text` `:offset` `:size`
Creates a checkbox inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`

#### `add-radio` `:parent` `:text` `:offset` `:size`
Creates a radio button inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `text` `[string!]` Label
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`
* `/group`
* `id` `[integer!]` Radios sharing an id turn each other off (default: 0)

#### `add-slider` `:parent` `:offset` `:size`
Creates a slider inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]` Taller than wide makes it vertical
* `/value`
* `val` `[percent! decimal!]` Initial position (default: 0%)

#### `add-progress` `:parent` `:offset` `:size`
Creates a progress bar inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`
* `/value`
* `val` `[percent! decimal!]` Initial position (default: 0%)

#### `add-drop-down` `:parent` `:items` `:offset` `:size`
Creates a drop-down list inside a window and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `items` `[block!]` Strings to offer
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]` Of the closed control; room for the list is added
* `/index`
* `n` `[integer!]` Item picked to start with, 1-based (default: none)

#### `add-panel` `:parent` `:offset` `:size`
Creates a panel - a widget which holds other widgets - and returns its handle
* `parent` `[handle!]` Window or panel to put it in
* `offset` `[pair!]` Position inside the client area
* `size` `[pair!]`
* `/edge` Draws a frame around it
* `/title`
* `text` `[string!]` Caption set into the frame; implies /edge


## Used handles and its getters / setters

#### __WINDOW__ - GUI window handle

```rebol
;Refinement       Gets                Sets                          Description
/title            string!             string!                       "Text shown in the title bar"
/size             pair!               pair!                         "Size of the client area in pixels"
/offset           pair!               pair!                         "Position of the top-left corner on the screen"
/id               integer!            none                          "Native window handle as an integer"
/open?            logic!              none                          "False once the window has been closed"
/font             string!             [string! none!]               "Font family widgets are created with; none for the system font"
/font-size        integer!            [integer! none!]              "Point size widgets are created with; none for the system size"
/bold?            logic!              logic!                        "Whether widgets are created bold"
/italic?          logic!              logic!                        "Whether widgets are created italic"
```

#### __WIDGET__ - GUI widget handle - a native control inside a window

```rebol
;Refinement       Gets                Sets                          Description
/text             string!             string!                       "Label or contents; the caption of a framed panel; the selected item of a drop-down, which is read-only; none for an image"
/items            block!              block!                        "Strings a drop-down offers; none for other kinds"
/index            integer!            integer!                      "Which item is picked, 1-based; 0 for none"
/image            image!              image!                        "Image shown by an image widget, none for other kinds"
/size             pair!               pair!                         "Size of the control"
/offset           pair!               pair!                         "Position inside whatever holds it - a window or a panel"
/id               integer!            none                          "Native control handle as an integer"
/kind             word!               none                          "What the control is: button, image, text, field, area, check, radio, slider, progress or drop-down"
/value            percent!            [percent! decimal!]           "Position of a slider or a progress bar; none for other kinds"
/state            logic!              logic!                        "Whether a check or a radio is on; none for other kinds"
/edge             logic!              logic!                        "Whether a panel draws a frame around itself; none for other kinds"
/font             string!             [string! none!]               "Font family; none puts it back to the system font"
/font-size        integer!            [integer! none!]              "Point size; none puts it back to the system size"
/bold?            logic!              logic!                        "Whether the text is bold"
/italic?          logic!              logic!                        "Whether the text is italic"
/color            tuple!              [tuple! none!]                "Text colour; none lets the platform decide"
/group            integer!            none                          "Which radio group it belongs to; 0 for everything else"
/enabled?         logic!              logic!                        "Whether the control responds to the user"
/parent           handle!             none                          "Whatever holds it - a window, or a panel; none once gone"
/window           handle!             none                          "The window it ends up in, however deeply nested"
```


## Other extension values:
```rebol
;; Bits carried by the fourth value of a mouse event. A `wheel` event
;; uses that slot for the signed number of lines instead, and `close`
;; and `resize` leave it at zero.
event-flags: object [
	shift:   1
	control: 2
	alt:     4
	double:  8
]

;; `poll-events` always returns a block, so the four values of every
;; event can be taken apart directly:
;;
;;     foreach [type source position value] poll-events [...]
;;
;; `source` is the window for window events, and the widget itself for a
;; `click` - use `source/parent` to get back to the window.
do-events: function [
	"Pumps window events until the given window is closed"
	window  [handle!]
	handler [any-function!] "Called as: handler type source position value"
	/rate delay [number!] "Idle time between polls (default: 0.01)"
][
	delay: any [delay 0.01]
	forever [
		foreach [type source pos val] poll-events [
			handler type source pos val
			;; `close` only reports the request - closing is ours to do
			if all [type = 'close  source/id = window/id] [
				close-window window
			]
			;; the handler is allowed to close it as well, and the rest
			;; of this batch would then refer to widgets which are gone
			unless window/open? [break]
		]
		unless window/open? [exit]
		wait delay
	]
]
```
