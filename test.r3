Rebol [
	Title:   "Rebol/GUI extension test"
	Needs:   3.22.10
	Purpose: {
		Checks what can be checked from a script, then opens the windows and
		prints the events they produce. The checks report as they go and the
		totals are printed when the main window is closed; the exit code is
		1 when any failed. What only a person can judge is marked WATCH.

		Usage:
			r3 test.r3
	}
]


print ["Running test on Rebol build:" mold to-block system/build]

;;=============================================================================
;; Minimal self-contained harness
;;=============================================================================

test-count: 0
fail-count: 0
skip-count: 0
failed: copy []

group: func ["Starts a named group of tests" name [string!]][
	print ajoin [lf as-yellow "== " as-yellow name]
]

;; NOTE: plain `func` with explicit locals - `function` would collect the
;; counter set-words as locals and the totals would never move.
;;
;; A test block must EVALUATE to true; anything else counts as a failure and
;; is printed as the detail, so a check can hand back a string saying what
;; went wrong. Never use `return` inside one - it would return from
;; `--test--` itself and the result would never be counted.
--test--: func [
	"Passes when the code block evaluates to true"
	name [string!]
	code [block!]
	/local result
][
	test-count: test-count + 1
	result: try code
	case [
		error? :result [
			fail-count: fail-count + 1
			append failed name
			print [as-red "[FAIL]" name]
			print [as-red "      " mold/flat :result]
		]
		:result = true [
			print [as-green "[ ok ]" name]
		]
		true [
			fail-count: fail-count + 1
			append failed name
			print [as-red "[FAIL]" name "=>" mold/flat/part :result 60]
		]
	]
	:result
]

--skip--: func ["Counts a test which cannot run here" name [string!]][
	skip-count: skip-count + 1
	print [as-purple "[skip]" name]
]

--rejects--: func [
	"Checks that the code is refused with an error"
	label [string!]
	code  [block!]
][
	--test-- ajoin [label " is refused"] compose/only [error? try (code)]
]

;; Compares a result with what it should be and hands back the actual value
;; when they differ, so the failure line shows it.
is?: func ["Compares a value with the expected one" value expected][
	any [value == expected  ajoin ["got " mold/flat/part value 60]]
]

summary: does [
	print ajoin [lf as-yellow "-----------------------------------------------------------------------------"]
	print [
		"tests:" test-count
		as-green ajoin ["passed: " test-count - fail-count - skip-count]
		either fail-count > 0 [as-red ajoin ["failed: " fail-count]][ajoin ["failed: " fail-count]]
		as-purple ajoin ["skipped: " skip-count]
	]
	if fail-count > 0 [
		print as-red ajoin ["^/Failed tests:^/  " mold/flat failed]
	]
	print ""
	quit/return either fail-count > 0 [1][0]
]

;;=============================================================================

;; make sure that we load a fresh extension
try [system/modules/gui: none]

;; a locally built library can be pointed at without installing it
if modules-dir: get-env 'REBOL_MODULES_DIR [
	system/options/modules: dirize to-rebol-file modules-dir
]

gui: import 'gui

;;=============================================================================
group "Helpers"
;;=============================================================================

;; `within?` - the box test every mouse handler below uses. Left and top
;; edges are in, right and bottom out.
--test-- "within? inside"          [within? 15x15 10x10 20x20]
--test-- "within? top-left edge"   [within? 10x10 10x10 20x20]
--test-- "within? right edge out"  [not within? 30x15 10x10 20x20]
--test-- "within? bottom edge out" [not within? 15x30 10x10 20x20]
--test-- "within? empty box"       [not within? 10x10 10x10 0x0]
--rejects-- "popup-menu on a non-handle" [popup-menu 42 []]
? gui

;; An area's text is plain LF on every platform: on Windows the extension
;; turns it into the CR LF the control needs, and back.
NL: LF

;;=============================================================================
group "Opening a window"
;;=============================================================================

;; /hidden, and shown again once the layout below is complete.
;;
;; Nothing paints until the pump runs, so widgets no longer appear one at a
;; time as they are created - but a window which is ALREADY on screen while
;; its layout is built still shows up empty first and fills in at the first
;; `wait`. Building it hidden is what makes it appear finished.
win: open-window/title/at/hidden 640x480 "Rebol GUI extension" 200x120

print ["window:  " win]
print ["id:      " win/id]
print ["offset:  " win/offset]
print ["scale:   " win/scale "(device pixels per unit - sizes below are logical)"]
--test-- "window is open"        [win/open?]
--test-- "window title"          [is? win/title "Rebol GUI extension"]
--test-- "window size"           [is? win/size 640x480]
--test-- "resizable? by default" [win/resizable?]
--test-- "title? by default"     [win/title?]
--test-- "scale is positive"     [win/scale > 0]

;; The icon, from an image!: a round spot with a transparent outside - the
;; alpha channel is used, 255 opaque. It is copied when set, so the image is
;; free to change afterwards; setting it again shows the change.
;;
;; WATCH: the title bar and the taskbar button (Windows, Linux X11) show the
;; bird from %icon.png; on macOS the Dock icon does. "Repaint" from the
;; canvas's context menu or the menu bar swaps it for a coloured disc, and
;; back again.
ico: make image! 32x32
repeat y 32 [repeat x 32 [
	d: (as-pair x y) - 16.5x16.5
	ico/(as-pair x y): either 225 > ((d/x * d/x) + (d/y * d/y)) [230.120.30.255][0.0.0.0]
]]
--test-- "no icon by default" [none? win/icon]
win/icon: ico
--test-- "icon set, the same image back" [same? win/icon ico]
win/icon: none
--test-- "icon removed" [none? win/icon]
--rejects-- "a string as an icon" [win/icon: "x"]
;; The real one, from a file next to this script. "Repaint" swaps it for the
;; disc in a new colour and back - an icon changes at runtime as often as
;; it is set.
bird: load %icon.png
win/icon: bird
recolor-icon: does [
	win/icon: either same? win/icon bird [ico/rgb: random 255.255.255  ico][bird]
]
;; Asked of the system each time; `theme-change` reports when it switches.
--test-- "dark? is a logic!" [logic? win/dark?]

;; Off by default: on Windows only the title bar follows the appearance. With
;; it on, the controls and every colour left at `none` follow as well.
;;
;; WATCH (Windows, dark system): every window starts light inside, with a
;; dark title bar. Turn the main window's controls dark from the View menu.
--test-- "dark-controls? off by default" [not win/dark-controls?]
win/dark-controls?: true
--test-- "dark-controls? on" [win/dark-controls?]
win/dark-controls?: false ;; turn it off again

;; `keys?` reports every key pressed in the window - whichever widget has the
;; focus, which is the event's source - without taking it from that widget.
;; `key` carries a char!, `named-key` a word; both have a `-up` pair.
;;
;; WATCH: keys typed in the main window appear in the log area.
--test-- "keys? off by default" [not win/keys?]
win/keys?: true
--test-- "keys? on" [win/keys?]

;; `caps-lock?` asks the system each time; it needs no window and no `keys?`.
;;
;; WATCH: with Caps Lock on, logged keys end with "(caps lock)".
--test-- "caps-lock? is a logic!" [logic? caps-lock?]

;; every accessor which can be read can also be written, except id and open?
win/title: "Rebol GUI extension - move the mouse"
--test-- "title set" [is? win/title "Rebol GUI extension - move the mouse"]
--rejects-- "setting open?" [win/open?: false]

;;=============================================================================
group "Native buttons"
;;=============================================================================

;; Both are children of the window's client area, positioned from its
;; top-left corner - the same convention everything else here uses.
;; A zero axis asks the control what it needs for its own text, in its
;; own font - which is the only thing that knows. Everything else here is
;; in LOGICAL units: 96 to the inch, the same number on any display.
counter: add-button win "Click me"  20x20 140x32
closer:  add-button win "Close it" 180x20 0x0

--test-- "button kind"     [is? counter/kind 'button]
--test-- "button text"     [is? counter/text "Click me"]
--test-- "button offset"   [is? counter/offset 20x20]
--test-- "button size"     [is? counter/size 140x32]
--test-- "button enabled?" [counter/enabled?]
--test-- "button parent is the window" [counter/parent == win]
--test-- "a zero size is measured" [all [closer/size/x > 0 closer/size/y > 0]]

;;=============================================================================
group "An image widget"
;;=============================================================================

;; The widget keeps a REFERENCE to this image, not a copy - so drawing into
;; `pic` and calling `redraw` is all it takes to change what is on screen.
;; This is the seam a rendering extension plugs into.
pic: make image! 240x160

paint: function ["Fills the image with a gradient" img [image!] shift [integer!]][
	repeat y img/size/y [
		repeat x img/size/x [
			img/(as-pair x y): as-color
				(255 * x / img/size/x)
				(255 * y / img/size/y)
				(shift % 256)
		]
	]
]
paint pic 0

canvas: add-image win pic 20x70

;; An image widget is a CONTAINER, like a panel: widgets given to it are
;; positioned inside it, clipped to it, and go away with it. Which is the
;; only way to put a caption ON the pixels - two overlapping siblings have
;; no defined order on Win32 and would fight over the same area.
;;
;; `transparent?` is what lets the image show through instead of a slab of
;; window colour, and the text colour is set to suit the picture.
caption: add-text canvas "on the image" 8x8 200x0
caption/transparent?: true
caption/color: 255.255.255
caption/bold?:  true

--test-- "caption's parent is the image"     [caption/parent == canvas]
--test-- "its window is still the window"    [caption/window == win]
--test-- "caption transparent?"              [caption/transparent?]
--test-- "a transparent one has no background" [none? caption/background]

;; `at` is where a widget sits in its WINDOW, however deeply nested - the
;; point mouse offsets are measured from. For something the window holds
;; directly it is just `offset`; one level down it adds up. A window's own
;; is 0x0, so `evt/offset - evt/source/at` works whatever the source.
--test-- "canvas/at is its offset"    [is? canvas/at canvas/offset]
--test-- "caption/at adds the canvas" [is? caption/at canvas/at + caption/offset]
--test-- "a window's at is 0x0"       [is? win/at 0x0]

;; Every container answers `children`, kept in the handle's own GC-marked
;; slot alongside whatever else that kind holds - the image! here, the menu
;; block on a window.
--test-- "the image holds one widget" [is? length? canvas/children 1]
--test-- "and it is the caption"      [caption == first canvas/children]
--test-- "a button has no children"   [none? counter/children]

--test-- "image kind" [is? canvas/kind 'image]
--test-- "image size" [is? canvas/size 240x160]
;; Reading the image back out of the widget. NEVER mold an image! into the
;; console - that is every pixel as text - so only its type and size.
;;
;; The size comes from the series itself, so a wrong answer here would mean
;; the accessor lost the dimensions on the way out.
--test-- "image read back"    [all [image? canvas/image  canvas/image/size = 240x160]]
--test-- "the same series"    [same? canvas/image pic]
--test-- "an image has no text" [none? canvas/text]

;;=============================================================================
group "Text widgets"
;;=============================================================================

;; A label, a one-line entry and a multi-line entry. All three carry their
;; string in the same `text` accessor the button uses.
label: add-text  win "Type your name:"  300x70  220x0
name:  add-field win ""                 300x100 240x0
log:   add-area  win "-- event log --"  300x140 300x200

--test-- "text kinds" [is? reduce [label/kind name/kind log/kind] [text field area]]
print ["natural heights - label:" label/size "field:" name/size "button:" closer/size]
--test-- "a zero height is measured" [all [label/size/y > 0  name/size/y > 0]]
--test-- "label text" [is? label/text "Type your name:"]

;; Writing to a control from Rebol does NOT come back as a `change` event -
;; only what the user types does.
name/text: "world"
--test-- "field text set" [is? name/text "world"]

;; The log is written to by the handler and by nobody else, so it is made
;; read-only: unlike `enabled?: false`, the text keeps its normal colours,
;; can still be selected and copied, and the area still scrolls.
;;
;; TRY IT: click in the log and type - nothing should happen - then select
;; some of it, which should still work.
log/read-only?: true
--test-- "log read-only?, still enabled" [all [log/read-only?  log/enabled?]]
--test-- "a label has no read-only?" [none? label/read-only?]

;; The two are separate questions, and on macOS they are one property - so
;; a disable/enable cycle must leave the read-only flag standing.
log/enabled?: false
log/enabled?: true
--test-- "read-only survives an enable cycle" [log/read-only?]

;; `scroll` is a percent, and takes one of `top`, `bottom` and `end` as well -
;; the words for the two positions anyone actually asks for. An area which
;; fits its own text has nowhere to go and answers 0%.
--test-- "an area which fits is at 0%" [is? log/scroll 0%]
--test-- "a field does not scroll"     [none? name/scroll]

;; Enough lines to need the scrollbar, then the three ways of moving it.
loop 40 [log/text: append log/text join NL "filler"]
--test-- "an area reads back LF, not CR LF" [not find log/text CR]
log/scroll: 'end
--test-- "scrolled to the end" [is? log/scroll 100%]
log/scroll: 'top
--test-- "back to the top" [is? log/scroll 0%]
log/scroll: 50%
--test-- "halfway" [all [log/scroll > 40%  log/scroll < 60%]]
log/text: "-- event log --"
log/scroll: 'top

;;=============================================================================
group "Typography"
;;=============================================================================

;; Anything with `text` has `font`, `font-size`, `bold?`, `italic?` and
;; `color`. The font is read back out of the CONTROL, so what is reported is
;; what is really on screen - including whatever the platform started it with.
print ["label started as:" mold label/font label/font-size "bold?" label/bold?]

was: label/size

label/font-size: 15
label/bold?:     true
label/color:     30.90.170
--test-- "font-size set" [is? label/font-size 15]
--test-- "bold? set"     [label/bold?]
--test-- "colour reads back" [is? label/color 30.90.170]

;; Changing only the family or the style keeps the size the font has, to the
;; fraction of a point. (On macOS the system's 13 units are 9.75 points, read
;; back as 10 - and set again as 10 they became 13.33 units: a label grew.)
sized: add-text win "Size kept" 400x5 0x0
was-size: sized/size
sized/font: sized/font
sized/size: 0x0
--test-- "the same family again, same size" [is? sized/size was-size]
remove-widget sized

;; `bold?` and `italic?` are one setting underneath and share one code path -
;; each must change only itself. (`bold?: true` once set italic instead.)
--test-- "bold, and only bold" [all [label/bold?  not label/italic?]]
label/italic?: true
--test-- "italic added, bold kept" [all [label/bold?  label/italic?]]
label/italic?: false
--test-- "italic off, bold kept" [all [label/bold?  not label/italic?]]

;; Nothing re-measures itself: a control which was told how big to be keeps
;; that size, whatever happens to its font, because the box a script laid out
;; is the box it meant. A bigger font in the old box clips.
--test-- "size unchanged by the font" [is? label/size was]

;; Asking for a re-fit is a ZERO AXIS in `size` - the same convention `add-*`
;; uses. `label` was created 220x0, so 220x0 is also how to say "keep the
;; width you were given, measure the height again".
label/size: 220x0
--test-- "re-fit: the measured height grew" [label/size/y > was/y]
--test-- "re-fit: the given width is kept"  [label/size/x = 220]

;; 0x0 measures both. The width of a label is its text, so this one ends up
;; snug around it - then back to a given width, which is how it is left for
;; the rest of the script.
label/size: 0x0
--test-- "0x0 measures both" [all [label/size/x > 0  label/size/y > 0]]
label/size: 220x0

;; A kind with no size of its own refuses to be asked, rather than quietly
;; doing nothing.
--rejects-- "asking an image to re-fit" [canvas/size: 0x0]

;; `none` puts a part back to whatever the platform uses.
log/font: "Courier New"          ;; a missing family falls back, never fails
log/font-size: 10
name/color: 150.30.30

;; Windows draws a push button's text itself, in the system colour, and only
;; an owner-drawn button could say otherwise. The value is still kept and
;; still reads back - it just does not show there. On macOS it does.
counter/color: 200.110.0
--test-- "button colour kept" [is? counter/color 200.110.0]

;; A window carries a default which widgets pick up AS THEY ARE CREATED.
;; Setting it does not reach back into what is already on screen.
win/font-size: 15
win/italic?:   true
styled: add-text win "made after the window default was set" 300x395 320x24

--test-- "window default set"     [all [win/font-size = 15  win/italic?]]
--test-- "the new label took it"  [all [styled/font-size = 15  styled/italic?]]
--test-- "the first one did not"  [not label/italic?]

;; Put it back, so the rest of this script builds ordinary widgets.
win/font-size: none
win/italic?:   false

;;=============================================================================
group "Checks and radios"
;;=============================================================================

toggle: add-check win "Counting enabled" 20x250 220x24
toggle/state: true

;; A panel holds other widgets, and what it holds is positioned inside IT -
;; these radios are at 10x26 within the panel, not within the window.
;;
;; /title gives it a frame with a caption - a group box. The frame is drawn
;; INSIDE the panel's own box and changes nothing about where a child sits:
;; 10x5 means the same thing framed or not, so leaving room for the frame is
;; the caller's job. Compare the two radios below, which are in the window.
box: add-panel/title win 20x285 260x60 "Temperature"

;; A panel is in the same family, so it takes a colour of its own - and the
;; radios inside it then need to be told to show it through, because a
;; control fills with the WINDOW's colour by default, not its parent's.
;; WATCH: the two radios below must sit on the panel's colour, with no pale
;; rectangle around either of them.
;; WATCH: the colour fills only the inside of the frame - the strip above
;; its top line, where the caption "Temperature" sits, is the window's.
;; A colour a script sets is the script's to change - see `dark-controls?`
;; below, which leaves it alone - so it is picked from the appearance here,
;; and again on every `theme-change`.
panel-color: func [dark [logic!]][either dark [48.52.62][235.240.250]]
box/background: panel-color did all [win/dark-controls? win/dark?]

;; Two independent groups. Radios turn each other off only within a group,
;; and the grouping is the extension's own - it does not depend on creation
;; order, on WS_GROUP flags, or on what AppKit considers a sibling. Note
;; that it does not depend on the panel either: warm and cool are in the
;; box, slow and fast are not, and the two groups still stand apart.
warm: add-radio/group box "Warm"  10x26 110x22 1
cool: add-radio/group box "Cool" 130x26 110x22 1
slow: add-radio/group win "Slow"  20x350 110x22 2
fast: add-radio/group win "Fast" 150x350 110x22 2

;; A separator. Taller than wide runs vertically; a zero axis is the line's
;; own thickness, so only the length has to be given.
;;
;; WATCH: a thin vertical rule between "Warm" and "Cool" in the panel.
rule: add-line box 124x24 0x26
--test-- "line kind"           [is? rule/kind 'line]
--test-- "line: its thickness" [all [rule/size/x > 0  rule/size/y = 26]]
--test-- "line's parent is the panel" [rule/parent == box]
--test-- "a line takes no focus"   [not set-focus rule]
--test-- "a line has no enabled?"  [none? rule/enabled?]
--test-- "a line has no text"      [none? rule/text]
--rejects-- "a line of 0x0" [add-line box 0x0 0x0]

warm/state: true
slow/state: true

;; Show the panel's colour through, rather than each radio filling its own
;; box with the window's. `false` puts it back, and `background` set to a
;; tuple would fill with that instead.
warm/transparent?: true
cool/transparent?: true
--test-- "radios are transparent" [all [warm/transparent?  cool/transparent?]]
--test-- "the panel's colour" [is? box/background panel-color did all [win/dark-controls? win/dark?]]

;; In the order they were added, and only the ones this panel holds - slow
;; and fast are in the window, so they are in ITS list instead.
--test-- "the panel holds radios and the line" [is? length? box/children 3]
--test-- "in creation order" [warm == first box/children]
print ["the window holds:" length? win/children "widget(s)"]
--test-- "a panel's radios are not in the window's list" [not find win/children warm]

--test-- "check kind and state" [all [toggle/kind = 'check  toggle/state]]
--test-- "radio kind, group and state" [all [warm/kind = 'radio  warm/group = 1  warm/state]]

;; The frame and its caption are readable and writable after the fact - both
;; are drawn by the extension at paint time, so neither rebuilds the control
;; and neither moves anything the panel holds.
--test-- "panel border?"  [box/border?]
--test-- "panel caption"  [is? box/text "Temperature"]
box/text: "Temperature (group box)"
--test-- "panel retitled" [is? box/text "Temperature (group box)"]
--test-- "a check has no border?" [none? toggle/border?]

;; `parent` is whatever holds it; `window` is the window either way.
--test-- "warm sits in the panel" [all [warm/parent == box  warm/offset = 10x26]]
--test-- "... and its window is the window" [warm/window == win]
--test-- "slow sits directly in the window" [slow/parent == win]

;; Turning one on turns its own group off - and leaves the other alone.
cool/state: true
--test-- "a radio turns its group off"  [all [cool/state  not warm/state]]
--test-- "the other group is untouched" [all [slow/state  not fast/state]]
warm/state: true

;; A toggle is a push button which stays pushed: on or off like a check,
;; drawn like a button. `state` reads and writes it, and a click reports
;; the state it has settled into.
;;
;; WATCH: "Bold log" stays pushed in after a click and makes the log bold;
;; a second click lets it out again.
bolder: add-toggle win "Bold log" 20x440 100x0
--test-- "toggle kind, off" [all [bolder/kind = 'toggle  not bolder/state]]
bolder/state: true
--test-- "toggle set from Rebol" [bolder/state]
bolder/state: false
--test-- "toggle back" [not bolder/state]
--test-- "a button has no state" [none? counter/state]

keys-switch: add-toggle win "Catch keys" 130x440 0x0
keys-switch/state: win/keys?

;;=============================================================================
group "Slider and progress"
;;=============================================================================

;; Both carry a `value` from 0% to 100%. A slider taller than it is wide
;; is vertical; these two are horizontal.
level: add-slider/value   win 20x375 240x20 25%
meter: add-progress/value win 20x410 240x10 25%

--test-- "slider"   [all [level/kind = 'slider    level/value = 25%]]
--test-- "progress" [all [meter/kind = 'progress  meter/value = 25%]]
--test-- "a progress bar has no enabled?" [none? meter/enabled?]

;; Taller than wide - no extra argument, just the proportions. `0%` is the
;; bottom on both platforms; Windows flips the trackbar's inverted axis so
;; the caller never sees it.
riser: add-slider/value win 265x70 20x160 25%
--test-- "vertical slider" [is? riser/value 25%]
riser/value: 100%
--test-- "vertical slider at the top" [is? riser/value 100%]
riser/value: 0%
--test-- "vertical slider at the bottom" [is? riser/value 0%]
riser/value: 25%

;;=============================================================================
group "Drop-list"
;;=============================================================================

;; The non-editable one - a button that drops a list. `size` is the closed
;; control; room for the list it drops is the backend's problem, not the
;; caller's.
picker: add-drop-list/index win
	["Bilberry" "Cloudberry" "Lingonberry" "Rowan"] 300x350 100x26 2

--test-- "drop-list kind"  [is? picker/kind 'drop-list]
--test-- "drop-list items" [is? picker/items ["Bilberry" "Cloudberry" "Lingonberry" "Rowan"]]
--test-- "drop-list index and text" [all [picker/index = 2  picker/text = "Cloudberry"]]

;; Replacing the list is a plain block assignment; the selection is dropped
;; with the items it referred to.
picker/items: ["Ash" "Birch" "Elm" "Oak" "Rowan"]
--test-- "replacing the items drops the pick" [is? picker/index 0]
picker/index: 4
--test-- "picked in the new items" [all [picker/index = 4  picker/text = "Oak"]]

;;=============================================================================
group "Drop-down"
;;=============================================================================

;; The editable one - a combo box: pick from the list, or type free text.
;; `text` is both readable and writable here, unlike a drop-list's.
;; WATCH (Windows): click into the typed text and press Tab - the focus
;; moves on. The typed part is an edit the combo box makes inside itself,
;; so it has to do the keyboard handling every other control does. With
;; `keys?` on, the keys typed there name the drop-down as their source.
;; WATCH (Windows): move the pointer over the typed text - the title bar
;; says "over drop-down", the log reports no `!!` line, and the tooltip
;; set below shows up there just as it does over the arrow button.
combo: add-drop-down/index win
	["Small" "Medium" "Large"] 410x350 100x0 1

--test-- "drop-down kind"  [is? combo/kind 'drop-down]
--test-- "drop-down items" [is? combo/items ["Small" "Medium" "Large"]]
--test-- "drop-down index and text" [all [combo/index = 1  combo/text = "Small"]]
combo/text: "Extra Large"
--test-- "drop-down text typed directly" [is? combo/text "Extra Large"]
--rejects-- "a drop-list's text" [picker/text: "x"]

;;=============================================================================
group "Text-list"
;;=============================================================================

;; The same list as a drop-list, shown in a box. It is kept short on purpose:
;; the items do not fit, so the vertical scroll bar shows up on its own.
trees: add-text-list win ["Ash" "Birch" "Elm" "Oak" "Rowan" "Willow"] 300x425 300x48

--test-- "text-list kind"  [is? trees/kind 'text-list]
--test-- "text-list items" [is? trees/items ["Ash" "Birch" "Elm" "Oak" "Rowan" "Willow"]]
--test-- "nothing picked"  [all [trees/index = 0  none? trees/text]]

trees/index: 3
--test-- "picked from Rebol" [all [trees/index = 3  trees/text = "Elm"]]
--rejects-- "a text-list's text" [trees/text: "Yew"]

;; Replacing the list drops the selection with the items it referred to.
trees/items: ["Alder" "Hazel" "Linden" "Maple" "Poplar" "Spruce" "Yew"]
--test-- "replaced items, pick dropped" [all [7 = length? trees/items  trees/index = 0]]
trees/index: 7            ;; the last one - scrolled into view
--test-- "the last one picked" [is? trees/text "Yew"]
trees/index: 0            ;; zero, or anything out of range, picks nothing
--test-- "pick cleared" [none? trees/text]

;; `scroll` works as for an area, a fraction of the way down.
trees/scroll: 'top
--test-- "text-list scrolled to the top" [is? trees/scroll 0%]
trees/scroll: 'end
--test-- "text-list scrolled to the end" [is? trees/scroll 100%]

;; An integer brings that item into view without picking it, scrolling as
;; little as it takes; out of range is clamped.
trees/scroll: 1
--test-- "item 1 in view, not picked" [all [trees/scroll = 0%  trees/index = 0]]
trees/scroll: 100
--test-- "past the end is clamped" [is? trees/scroll 100%]
--rejects-- "an integer scroll on an area" [log/scroll: 1]

;; `scrollable?` off takes the scroll bar and the wheel away from the user;
;; code still scrolls it.
--test-- "scrollable? by default" [trees/scrollable?]
trees/scrollable?: false
--test-- "scrollable? off" [not trees/scrollable?]
trees/scroll: 'top
--test-- "still scrolled by code" [is? trees/scroll 0%]
trees/scrollable?: true
--test-- "not a drop-list's accessor" [none? picker/scrollable?]

;; `/index` picks one at creation, as for a drop-list; a zero size asks the
;; list for its natural one. This one is removed straight away.
spare: add-text-list/index win ["one" "two"] 0x0 0x0 2
--test-- "/index picks at creation" [is? spare/text "two"]
--test-- "a zero size is measured"  [all [spare/size/x > 0  spare/size/y > 0]]
remove-widget spare
release spare

logged: copy "-- event log --"
note: func ["Appends a line to the area" line [string!]][
	logged: log/text
	append logged join NL line
	log/text: logged
	;; Setting the text puts a Win32 edit control back at the top, so the
	;; newest line is only visible if the area is told to follow it.
	log/scroll: 'end
]


;;=============================================================================
group "Tooltips"
;;=============================================================================

;; The platform's own tooltip - its delay, its placement, its look. Any kind
;; can have one; `none` (or an empty string) takes it away again.
;;
;; WATCH: rest the pointer on "Click me", on the picture, on the label above
;; the field and on the slider - each shows its tip. A label is a special
;; case on Windows (it never sees the mouse itself), so it is worth checking.
--test-- "no tip to begin with" [none? counter/tip]
counter/tip: "Counts the clicks and repaints the picture"
canvas/tip:  "Drop an image file here to show it"
label/tip:   "Greets whoever types in the field below"
level/tip:   "Drives the progress bar below it"
name/tip:    "Type your name - Enter reports a click"
log/tip:     "Read-only: select and copy, but no typing"
combo/tip:   "Pick one, or type your own"
--test-- "a tip reads back"     [is? counter/tip "Counts the clicks and repaints the picture"]
--test-- "a label has one too"  [is? label/tip "Greets whoever types in the field below"]

;; Replaced, not added to...
counter/tip: "Counts the clicks^/and repaints the picture"
--test-- "a tip replaced, two lines" [is? counter/tip "Counts the clicks^/and repaints the picture"]

;; ... and taken away with either `none` or an empty string.
closer/tip: "about to go"
closer/tip: none
--test-- "a tip removed with none" [none? closer/tip]
closer/tip: "about to go"
closer/tip: ""
--test-- "a tip removed with an empty string" [none? closer/tip]
--rejects-- "an integer tip" [closer/tip: 42]

;;=============================================================================
group "Showing the finished layout"
;;=============================================================================

;; Everything above was created while the window was hidden, and nothing
;; painted: each `add-*` only invalidated its control. This is where the
;; whole layout arrives, in one frame.
;;
;; WATCH THE SCREEN HERE: the window must appear complete. Widgets showing up
;; one after another - or an empty window that fills in a moment later - means
;; something is still forcing a paint per widget.
show-window win
print "the window is up - it should have arrived with everything on it"

;;=============================================================================
group "The keyboard focus"
;;=============================================================================

;; `focused?` is asked of the platform rather than remembered, because focus
;; moves for reasons this extension never hears about - a click, the window
;; being activated, another application taking over.
--test-- "the field takes the focus" [set-focus name]
--test-- "and reports it"            [name/focused?]
--test-- "the button does not"       [not counter/focused?]

--test-- "a button takes it too"     [set-focus counter]
--test-- "and the field has lost it" [not name/focused?]

;; A progress bar is not something a user can reach.
;; Refused by KIND, before the platform is asked, because the platforms
;; disagree: Win32's SetFocus works on any enabled window and would take a
;; progress bar or a label, showing nothing and doing nothing with a
;; keystroke, while AppKit refuses both. One answer is more use than two.
--test-- "a progress bar refuses the focus" [not set-focus meter]
--test-- "and never reports it"             [not meter/focused?]
--test-- "nor does a label"                 [not set-focus label]
--test-- "nor a panel"                      [not set-focus box]

;; Left on the field, which is where a typist wants it.
set-focus name

;;=============================================================================
group "Moving a widget over a sibling"
;;=============================================================================

;; The area a widget vacates when it moves or shrinks belongs to the WINDOW,
;; which paints its background across it - and that covers any sibling living
;; there, whose own control Windows still considers valid and would not
;; otherwise repaint.
;;
;; What makes it visible is the FIELD's sunken border. That border is in the
;; control's non-client area, drawn on WM_NCPAINT, and invalidating a client
;; area never raises one - so a border painted over stays painted over, which
;; is what "not fully redrawn" looked like.
;;
;; WATCH THE TOP EDGE OF THE FIELD: the label grows down across it and shrinks
;; back, and the field's frame must be unbroken afterwards.
field-box: reduce [name/offset name/size]
was: label/size

label/font-size: 20        ;; the same thing the `big` menu item does
label/size: 220x0          ;; ... and re-fit, which now reaches the field
--test-- "the label grew" [label/size/y > was/y]
grown: label/size
wait 0.2

label/font-size: none      ;; `normal` again
label/size: 220x0
--test-- "and shrank back" [label/size/y < grown/y]
wait 0.2

--test-- "the field is untouched"   [is? name/text "world"]
--test-- "and keeps its own box"    [is? reduce [name/offset name/size] field-box]

;;=============================================================================
group "The GUI device"
;;=============================================================================

;; The extension registers a device with RDO_AUTO_POLL, which is what makes
;; `wait` pump the OS message queue. Any positive id means the host accepted
;; it; a refusal would be one of the negative RDR_ codes.
--test-- "the host accepted the device" [gui-device > 0]

;; Polled from OS_Wait, so a plain `wait` must move the counter. This is half
;; the event model in one assertion: if it does not move, the window stops
;; responding whenever Rebol is waiting rather than polling.
;;
;; And it must move by MORE than one: WAIT calls OS_Wait repeatedly inside a
;; single wait, which is why the window keeps drawing and tracking the mouse
;; for the whole time `do-events` may be asleep for.
polls: gui-device-polls
wait 0.2
--test-- "the host polls the device" [gui-device-polls > polls]
--test-- "and more than once"        [(gui-device-polls - polls) > 1]

;; The other half: the device has a port, and pushes an event to it when
;; there is something to drain. `read` on it reports how many - the request
;; goes through the device's own command table, so a number coming back is
;; also proof that RDC_OPEN reached it rather than being a no-op.
--test-- "the event port is open" [port? gui/event-port]
poll-events                      ;; drop whatever the sections above queued
--test-- "nothing waiting after a drain" [zero? read gui/event-port]

;; A quiet wait must SLEEP. A device cannot ask to be woken by its return
;; code - a non-zero answer from a poll makes the host attach the REBREQ it
;; lent off its own C stack to the device's pending list - so the ONLY thing
;; that may shorten a wait is a pushed event.
;;
;; "Quiet" means the mouse is off the window: a `move` event is an event like
;; any other and will ring the doorbell, which is the point. So this is only
;; reported, not counted.
start:   now/precise
wait 0.2
elapsed: difference now/precise start
print ["a quiet 0.2 wait took:" elapsed "- it must sleep, not spin (shorter if the mouse is over a window)"]

;; ...and a wait with an event pushed into it must RETURN EARLY. Resizing the
;; window from the program side produces one without touching the mouse: the
;; OS reports the change back like any other event. Note it is queued from
;; inside `win/size:`, NOT during a pump - which is why the doorbell asks
;; "anything waiting?" rather than "anything new?".
events: gui-device-events
win/size: win/size + 1x1

start:   now/precise
wait reduce [gui/event-port 2]   ;; two seconds it must not take
elapsed: difference now/precise start
--test-- "a wait on the port is woken early" [elapsed < 0:0:1]
--test-- "the device pushed the event"       [gui-device-events > events]
--test-- "and it is waiting"                 [(read gui/event-port) > 0]

;; One push per batch, however many polls go by: a second wake for an event
;; nobody has drained yet would be one more unhandled event on the system
;; port every time the host looks.
events: gui-device-events
wait 0.3
--test-- "no second push for the same batch" [gui-device-events = events]
print ["drained:" length? poll-events "event(s)"]
--test-- "the doorbell is armed again" [zero? read gui/event-port]


;;=============================================================================
group "Screens"
;;=============================================================================

;; One handle per display, the primary first. Every read asks the platform
;; again, so a handle always describes the display as it is NOW - and one
;; whose display has been unplugged reads none for everything.
all-screens: screens
--test-- "at least one screen" [not empty? all-screens]
foreach scr all-screens [
	print [
		pad mold scr/name 28
		"size" pad scr/size 10 "at" pad scr/offset 10
		"work" pad scr/work-size 10 "at" pad scr/work-offset 10
		"scale" scr/scale
		either scr/primary? ["primary"][""]
	]
]

main: first all-screens
--test-- "the first one is the primary" [main/primary?]
primaries: 0
foreach scr all-screens [if scr/primary? [primaries: primaries + 1]]
--test-- "there is exactly one primary" [is? primaries 1]

;; The same display is the same HANDLE, however it was reached - so `==`
;; answers "is this the same screen?" as it does for windows and widgets.
;; (`=` would not: on handles it compares only the type.)
--test-- "asking twice gives one handle" [main == first screens]
on-screen?: func [scr][
	foreach s screens [if s == scr [return true]]
	false
]
--test-- "the window knows its screen" [on-screen? win/screen]

;; Everything is in the space a window's `offset` uses, so the two can be
;; compared directly: the window's top-left corner is on its own screen.
home: win/screen
--test-- "the window is on it" [within? win/offset home/offset home/size]

;; The work area is the part not covered by the taskbar, the Dock or the
;; menu bar - inside the screen, and never bigger than it.
--test-- "the work area is inside the screen" [did all [
	within? home/work-offset home/offset home/size
	home/work-size/x <= home/size/x
	home/work-size/y <= home/size/y
]]

;; A window's scale IS its screen's - on Windows too, where the process is
;; per-monitor DPI aware (Windows 10 1703 and later; older systems report
;; the system scale for every screen).
--test-- "the window's scale is its screen's" [win/scale = home/scale]

;; A word this extension does not know still reaches the core's own answer.
--test-- "a screen handle's type" [is? main/type 'GUI-SCREEN]

;;=============================================================================
group "Window frames"
;;=============================================================================

;; `/fixed` opens a window the user cannot resize, and `/borderless` one with
;; no title bar and no frame at all. Both are readable and writable afterwards
;; - and changing either keeps the CLIENT size, so nothing inside moves.
fixed: open-window/title/at/fixed 240x120 "Fixed size" 700x120
--test-- "a /fixed window is not resizable" [all [not fixed/resizable?  fixed/title?]]
--test-- "its client size" [is? fixed/size 240x120]

;; `/flat` makes an entry or a list without its border - a plain box of text.
;; `border?` reads it back, and turns it on and off afterwards; the box stays
;; where it is.
;;
;; WATCH: in the "Fixed size" window, a field and an area with no border.
plain: add-field/flat fixed "a flat field" 10x10 220x0
;; WATCH (Windows): Tab from the flat field into this area and out again -
;; the focus moves on and no tab character is typed into it. The host
;; translates keys before dispatching them, so Tab's character is already
;; queued for the area when the key moves the focus away.
sheet: add-area/flat  fixed "a flat area^/with two lines" 10x44 220x66

;; WATCH: a horizontal rule between the flat field and the flat area.
hr: add-line fixed 10x38 220x0
--test-- "a horizontal line: its length, a thickness" [all [hr/size/x = 220  hr/size/y > 0]]
--test-- "/flat: no border" [all [not plain/border?  not sheet/border?]]
--test-- "a normal field has one" [name/border?]
plain/border?: true
--test-- "border? turned on" [plain/border?]
plain/border?: false
--test-- "border? turned off" [not plain/border?]

;; A window can carry a colour of its own, which every widget on it then
;; resolves to - a transparent label on a dark window needs no colour of its
;; own, only a light text colour.
tinted: open-window/title/at 260x120 "Dark window" 700x430
tinted/background: 24.26.34
note-dark: add-text tinted "on a dark window" 12x12 230x0
note-dark/transparent?: true
note-dark/color: 225.228.235
--test-- "window background" [is? tinted/background 24.26.34]
--test-- "a transparent label on it" [note-dark/transparent?]

;; `/secure` masks what is typed. The script still reads the real text.
;;
;; WATCH: dots in the "Dark window", not "hunter2"; copying out is refused.
secret: add-field/secure tinted "hunter2" 12x44 230x0
--test-- "secure? field, the real text" [all [secret/secure?  secret/text = "hunter2"]]
--test-- "an ordinary field is not secure" [not name/secure?]
--test-- "a button has no secure?" [none? counter/secure?]


;; And one which is SEE-THROUGH: the client area is dropped by the compositor
;; and only the widgets are left on screen.
;;
;; WATCH: the button below should appear to float over whatever is behind the
;; window. Borderless as well, because a window with nothing to grab is less
;; confusing than a frame around a hole.
ghost: open-window/at/borderless/transparent 240x80 700x580
float: add-button ghost "floating" 20x20 200x36
--test-- "a transparent window, no background" [all [ghost/transparent?  none? ghost/background]]

;; Turning it off makes it a solid window again, and back on again after.
ghost/transparent?: false
--test-- "solid again" [not ghost/transparent?]
wait 0.5
ghost/transparent?: true

bare: open-window/at/borderless 240x120 700x280
--test-- "a borderless window: no title bar" [not bare/title?]
--test-- "its client size is unchanged by having no frame" [is? bare/size 240x120]
print ["bare window resizable?" bare/resizable?]

;; A borderless window has no close box and nothing to drag, so the program
;; is the only thing that can move or close it. Give this one a way out.
add-text   bare "No title - and no way to close me" 10x10 220x0
back-again: add-button bare "Give me a frame" 10x50 0x0
;; `/borderless` means nothing around it at all - no outline, no shadow -
;; and that is kept when the button gives the title bar back and takes it
;; away again. A titled window always reads as having a border.
--test-- "a /borderless window has no border?" [not bare/border?]
--test-- "a titled window always has one"      [fixed/border?]
;; ... and a toggle next to the button switches it. It shows only while
;; there is no title bar, but the setting is kept either way.
bordered: add-toggle bare "Border" 150x50 0x0

;; A date-field: `value` is a date!, with the time of day when the field is
;; made with `/time`. Both start at now.
;;
;; WATCH: a date and time entry under the button in the borderless window.
;; Not on the "Dark window": its background is set by hand, and the macOS
;; stepper arrows are drawn for the system's light or dark look, not for it.
when: add-date-field/date/time bare 10x88 0x0 24-Dec-2026/18:30
--test-- "date-field kind" [is? when/kind 'date-field]
print ["date-field size:" when/size]
--test-- "date-field value" [when/value = 24-Dec-2026/18:30]
when/value: 31-Dec-2026            ;; no time given - the time of day is kept
--test-- "a date set keeps the time" [when/value = 31-Dec-2026/18:30]
when/value: 1-Jan-2027/9:15
--test-- "a date and time set" [when/value = 1-Jan-2027/9:15]
plain-date: add-date-field bare 10x88 0x0
--test-- "without /time, a plain date of today" [all [none? plain-date/value/time  plain-date/value = now/date]]
remove-widget plain-date
release plain-date

;; Turning the border back on does not turn resizing back on: they are two
;; properties, and this window never had the second one.
fixed/resizable?: true
--test-- "resizable? set on a /fixed window" [fixed/resizable?]

;;=============================================================================
group "Tab-panel"
;;=============================================================================

;; A page per tab, made for you: a panel each, in `children`, with `group`
;; saying which tab it is. Widgets go on the pages like on any panel.
;;
;; WATCH: a "Tabs" window with three tabs. Clicking one shows its page and
;; logs `change on tab-panel`; Tab reaches the tabs and the arrows switch.
tabbed: open-window/title/at 300x180 "Tabs" 1000x120
tabs:   add-tab-panel/index tabbed ["General" "Advanced" "About"] 10x10 280x160 2
pages:  tabs/children
--test-- "tab-panel kind and labels" [all [tabs/kind = 'tab-panel  tabs/items = ["General" "Advanced" "About"]]]
--test-- "/index shows the tab" [all [tabs/index = 2  tabs/text = "Advanced"]]
--test-- "a page per tab" [is? length? pages 3]
--test-- "a page is a panel with its tab as group" [all [pages/2/group = 2  pages/2/kind = 'panel]]
--test-- "a page's parent is the tab-panel" [pages/1/parent == tabs]

add-check pages/1 "Enabled" 10x10 0x0
add-field pages/2 "on the second page" 10x10 200x0
add-text  pages/3 "Rebol/GUI" 10x10 0x0

tabs/index: 1
--test-- "switched from Rebol (no `change` reported)" [all [tabs/index = 1  tabs/text = "General"]]
tabs/index: 9
--test-- "out of range keeps it" [is? tabs/index 1]
--rejects-- "setting a tab-panel's labels" [tabs/items: ["x"]]
--rejects-- "setting a tab-panel's text"   [tabs/text: "x"]
--rejects-- "a tab-panel with no tabs"     [add-tab-panel tabbed [] 0x0 10x10]

;;=============================================================================
group "List-view"
;;=============================================================================

;; A table: titles with optional widths, and the cells as one flat block, row
;; by row. Any values - they are shown as FORM shows them, and only when they
;; are about to be seen.
;;
;; WATCH: a "Table" window with three columns and five rows, the second one
;; picked. Picking a row logs `change`; a double click or Enter logs `click`;
;; clicking a header sorts by that column - again for descending - and shows
;; the arrow. `none` in a row shows as an empty cell.
files-data: [
	"readme.txt"    1200  1-Jan-2026
	"logo.png"     48213  2-Feb-2026
	"build.r3"       640  3-Mar-2026
	"notes"         none  4-Apr-2026
	%data/file.bin 99999  5-May-2026/12:30
]
table: open-window/title/at 360x200 "Table" 1000x340
files: add-list-view/with/index table ["Name" 140 "Size" 70 right "Date" none center] 10x10 340x180 files-data 2

--test-- "list-view kind" [is? files/kind 'list-view]
;; A width of none reads back as none!, which molds as `_`.
--test-- "columns read back" [is? files/columns reduce ["Name" 140 "Size" 70 'right "Date" none 'center]]
--test-- "five rows" [is? (length? files/items) / 3 5]
--test-- "items are the very block given" [same? files/items files-data]
--test-- "/index picks a row; text is its first cell" [all [files/index = 2  files/text = "logo.png"]]
files/index: 5
--test-- "a file! cell is formed" [is? files/text "data/file.bin"]
files/index: 0
--test-- "nothing picked" [all [files/index = 0  none? files/text]]
--rejects-- "half a row"                [files/items: ["x" 1]]
--rejects-- "half a row at creation"    [add-list-view/with table ["A" "B"] 0x0 10x10 [1 2 3]]
--test-- "no cells at all is fine"      [not error? try [remove-widget add-list-view table ["A" "B"] 0x0 10x10]]
--rejects-- "a bad column spec"         [add-list-view table [10 "A"] 0x0 10x10]
;; `none` in a literal block is the word - accepted as "fit the title", like
;; a reduced none! value.
--test-- "the word none as a width"     [not error? try [remove-widget add-list-view table ["A" none] 0x0 10x10]]
--test-- "a none! value as a width"     [not error? try [remove-widget add-list-view table reduce ["A" none] 0x0 10x10]]
--rejects-- "another word as a width"   [add-list-view table ["A" auto] 0x0 10x10]
--rejects-- "a list-view's text"        [files/text: "x"]
files/sort-column: -2
--test-- "sort arrow" [is? files/sort-column -2]
files/sort-column: none
--test-- "no arrow" [none? files/sort-column]
append files-data ["zzz.log" 1 6-Jun-2026]
files/items: files-data
--test-- "a row appended" [is? (length? files/items) / 3 6]
files/index: 6
--test-- "the new row picked" [is? files/index 6]

;; A number of columns set again keeps the cells when it is the same...
files/columns: ["File" none "Bytes" 80 right "When" center 120]
--test-- "renamed columns, rows kept" [is? (length? any [files/items []]) / 3 6]
;; none on a column which is not the last fits its title: an integer then.
--test-- "renamed columns read back" [
	all [
		files/columns/1 = "File"  integer? files/columns/2
		(skip files/columns 2) = reduce ["Bytes" 80 'right "When" 120 'center]
	]
]
--test-- "read back and set again, the same" [(mold files/columns) = (files/columns: files/columns  mold files/columns)]
--rejects-- "two widths"     [files/columns: ["A" 10 20]]
--rejects-- "two alignments" [files/columns: ["A" left right]]
--rejects-- "an unknown word in columns" [files/columns: ["A" middle]]
;; WATCH (Windows): the Explorer look - the row under the pointer is
;; highlighted, the picked row is a filled bar across all columns with no
;; dotted focus rectangle in it.
;; WATCH: Size right-aligned, Date centred, and the Date column reaching the
;; right edge of the list; dragging the Size edge keeps it reaching it.
files/columns: ["Name" 140 "Size" 70 right "Date" none center]

;; Stripes are `background` with two colours - the rows alternate. One colour,
;; or none, takes them off again. The first of the two may be none.
files/background: [none 235.240.250]
--test-- "stripes" [is? files/background reduce [none 235.240.250]]
files/background: 250.250.250
--test-- "one colour" [is? files/background 250.250.250]
--rejects-- "three colours" [files/background: [1.1.1 2.2.2 3.3.3]]
files/background: [235.240.250 none]
--test-- "stripes reversed" [is? files/background reduce [235.240.250 none]]
files/background: [none none]
--test-- "none none is no stripes" [none? files/background]
--rejects-- "one colour in a block" [files/background: [1.1.1]]
--rejects-- "another word in stripes" [files/background: [1.1.1 auto]]
--rejects-- "stripes on another kind" [counter/background: [1.1.1 2.2.2]]
;; WATCH (Windows, dark system theme): the table starts light, like the main
;; window. "Dark controls" from the menu switches both: the table's rows go
;; dark grey with light text, the header dark with light titles, the scroll
;; bar dark - and back again.
;; WATCH: the table's rows striped - light blue, or a lighter grey when dark;
;; the picked row and the hover still show over the stripes.
;; A stripe to suit the appearance, from the first row: the second colour
;; none, so every other row keeps the platform's own - light or dark.
stripes: func [dark [logic!]][either dark [[55.55.60 none]][[235.240.250 none]]]
files/background: stripes did all [table/dark-controls? table/dark?]

;; A width of 0 hides a column: its values stay in the rows - a row is as
;; long as the whole spec - but nothing shows them. `text` is still the first
;; value of the picked row, hidden or not, and `sort` reports positions in the
;; row, hidden ones counted.
hid: add-list-view/with table ["id" 0 "Name" 100 "path" 0] 0x0 10x10 [
	101 "readme" %docs/readme.txt
	102 "logo"   %img/logo.png
]
--test-- "hidden columns read back" [is? hid/columns ["id" 0 "Name" 100 "path" 0]]
--test-- "rows: three values a row, two hidden" [is? (length? hid/items) / 3 2]
hid/index: 2
--test-- "text is the hidden id" [is? hid/text "102"]
--rejects-- "half a row with hidden columns" [hid/items: [1 "x"]]
hid/columns: ["id" 0 "Name" none "path" 0]
;; Name is the last SHOWN column, so its none fills.
--test-- "fill skips hidden columns" [is? hid/columns reduce ["id" 0 "Name" none "path" 0]]
remove-widget hid

;; No titles at all: no header row. The header comes back with a title.
bare-lv: add-list-view/with table ["" 60 "" none] 0x0 10x10 [1 2]
--test-- "untitled columns read back" [is? bare-lv/columns reduce ["" 60 "" none]]
remove-widget bare-lv
;; ... and clears them when it is not.
spare-lv: add-list-view/with table ["One" "Two"] 0x0 10x10 [1 2 3 4]
spare-lv/columns: ["Only"]
--test-- "another number of columns clears the items" [none? spare-lv/items]
remove-widget spare-lv

;;=============================================================================
group "Scroller"
;;=============================================================================

;; A panel which scrolls. Its children are placed in its content, from the
;; content's top-left corner, and what there is to scroll is the box they
;; cover. `scroll` is what of the content is at the scroller's top-left.
;;
;; WATCH: a "Scroller" window with the bird at twice its size, in a box with
;; both scroll bars. The bars, the wheel (Shift for across) and a touchpad
;; scroll it, and the panel with the button and the check goes along, and
;; they still work:
;; clicking them logs `click`, the check ticks. No `!! move` notes in
;; the log while moving over it, whatever it is scrolled to - a child's `at`
;; follows the scrolling.
;; WATCH (macOS): the bars - the narrow, small ones - are there while there
;; is something to scroll, and can be dragged, whatever "Show scroll bars" is
;; set to.
;; WATCH: resizing the window resizes the scroller with it. Made bigger than
;; the bird, the bars go and the bird stays in the middle (`centered?`), on
;; the scroller's colour - with no frame lines around or near it; smaller
;; again, it scrolls from its corner.
;; WATCH: switch the system between light and dark - the scroller and the
;; panel on the picture change colour with it, and the bars, the button's
;; text and the check's label stay readable in both.
viewer: open-window/title/at/hidden 300x240 "Scroller" 1000x580
;; Its controls follow the appearance on Windows too, as they always do on
;; macOS and Linux - so the colours below can follow it the same way.
viewer/dark-controls?: true
sc:     add-scroller viewer 10x10 280x220
big:    add-image/size sc bird 0x0 512x512
;; ON the picture means IN the image widget: as siblings overlapping it, the
;; image would take their clicks on Windows. And in a panel there: a control
;; is drawn for the light or the dark appearance whatever is behind it - on
;; macOS its bezel even blends with it - so on the bird's dark background, in
;; the light appearance, it would come out dark, black text and all.
tools: add-panel/border big 20x20 170x80
add-button tools "On the picture" 10x10 0x0
add-check  tools "And this" 10x46 0x0
--test-- "scroller kind, one child" [all [sc/kind = 'scroller  1 = length? sc/children]]
--test-- "the picture holds a panel" [all [1 = length? big/children  big/children/1/kind = 'panel]]
--test-- "the panel holds two" [is? length? tools/children 2]
--test-- "starts at the top-left" [is? sc/scroll 0x0]
--rejects-- "a scroller with no size" [add-scroller viewer 0x0 0x0]

sc/scroll: 100x50
--test-- "scrolled from Rebol" [is? sc/scroll 100x50]
--test-- "the offset stays" [is? big/offset 0x0]
--test-- "the at moves" [is? big/at -90x-40]
;; As far as it goes: the picture less the part which shows - a little less
;; where the scroll bars take room of their own.
sc/scroll: 9999x9999
--test-- "clamped to the end, about 232x292" [any [within? sc/scroll 232x292 25x25  ajoin ["got " sc/scroll]]]
sc/scroll: 'top
--test-- "top keeps it across" [all [sc/scroll/y = 0  sc/scroll/x > 0]]
sc/scroll: 'bottom
--test-- "bottom keeps it across" [all [sc/scroll/y > 0  sc/scroll/x > 0]]
sc/scroll: -5x-5
--test-- "clamped to the start" [is? sc/scroll 0x0]
--rejects-- "a percent scroll on a scroller" [sc/scroll: 50%]

;; A child further out widens what there is to scroll; removing it, or making
;; one smaller, narrows it again - and the position with it.
far: add-button sc "Far" 900x10 0x0
sc/scroll: 9999x0
--test-- "a child further out widens it" [sc/scroll/x > 600]
remove-widget far
--test-- "removed, back within the picture" [sc/scroll/x <= 260]
big/size: 200x150
--test-- "all of it fits, nothing to scroll" [is? sc/scroll 0x0]
big/size: 512x512
--test-- "a child of a scroller is a child" [big/parent == sc]

;; `centered?` puts content smaller than the visible part in the middle of
;; it, on that axis. The children keep their offsets; their `at` says where
;; they ended up. Off by default, and only a scroller has it.
--test-- "centered? off by default" [not sc/centered?]
--test-- "no centered? on other kinds" [none? counter/centered?]
--rejects-- "centered? on a button" [counter/centered?: true]
sc/centered?: true
;; Colours of our own, for the appearance the controls are drawn for: a light
;; one in the dark appearance leaves white text and white scroll bars on it,
;; and a dark one in the light appearance dark buttons. Set again on every
;; `theme-change`, in `report`.
viewer-colors: func [dark [logic!]][
	either dark [[46.50.58 58.62.72]][[225.230.238 235.238.242]]
]
color-viewer: function [][
	colors: viewer-colors did all [viewer/dark-controls? viewer/dark?]
	sc/background:    colors/1
	tools/background: colors/2
]
color-viewer
--test-- "a scroller has a background" [is? sc/background first viewer-colors viewer/dark?]

;; The scroller fills its window - kept so on every `resize`, in `report`.
fit-viewer: does [sc/size: viewer/size - 20x20]
fit-viewer
--test-- "bigger than the scroller, from its corner" [is? big/at 10x10]
viewer/size: 700x600
fit-viewer   ;; what its `resize` will do too, once it is polled
--test-- "smaller, centred" [is? reduce [big/at big/offset sc/scroll] [94x44 0x0 0x0]]
viewer/size: 800x700
fit-viewer
--test-- "resized, centred again" [is? big/at 144x94]
viewer/size: 700x600
fit-viewer
sc/centered?: false
--test-- "not centred" [is? big/at 10x10]
sc/centered?: true
viewer/size: 300x240
fit-viewer
show-window viewer


;;=============================================================================
group "Tree-view"
;;=============================================================================

;; The menu dialect's grammar: a label, a word naming the node if it needs
;; one, and a block of children if it has any. A node is found by its PATH -
;; each node's word on the way down, or its label where it has no word - and
;; a node at the top is just its word or label. A block with the same values
;; is taken as well.
;;
;; WATCH: picking a node logs `change`; a double click or Enter logs `click`;
;; opening and closing a branch - with the mouse or Left and Right - logs
;; `open` and `close`, with the node's number in `tree/nodes` as the code.
;; A branch opened inside a closed one stays open when its parent opens.
;;
;; An image! - or a get-word holding one - after the label (and the word)
;; is the node's icon, scaled to
;; fit the row's height - `row-height` makes it bigger. One image! shown by
;; many nodes is stored once. A node without one keeps its label in line.
icon: func [color [tuple!] /local img] [
	img: make image! [32x32 0.0.0.0]          ;; transparent
	repeat y 26 [repeat x 26 [poke img as-pair x + 2 y + 2 color]]
	repeat y 24 [repeat x 24 [poke img as-pair x + 3 y + 3 color + 50.50.50]]
	img
]
folder: icon 200.150.0
file:   icon 80.120.200
trees: open-window/title/at 470x420 "Tree" 1000x420
tree: add-tree-view trees [
	"Documents" docs :folder [
		"Report.txt" report :file
		"Old" old :folder ["a.txt" a :file  "b.txt"]
	]
	"Music" music :folder ["Song.mp3" :file]
	"Notes.txt"
] 10x10 220x240
big-tree: add-tree-view trees [
	"Pictures" :folder ["Cat.png" :file  "Dog.png" :file]
] 240x10 220x240
big-tree/row-height: 40
big-tree/expanded: ["Pictures"]

;; A text-list takes an icon after an item's string, a list-view as a row's
;; first cell - the same image! or get-word.
icon-list: add-text-list/index trees ["Docs" :folder  "a.txt" :file  "plain"] 10x260 220x150 2
--test-- "a text-list with icons: its text" [is? icon-list/text "a.txt"]
--test-- "items read back the strings only" [is? icon-list/items ["Docs" "a.txt" "plain"]]
icon-view: add-list-view/with/index trees ["" 28  "Name" 100  "Size" none right] 240x260 220x150 [
	:folder "docs"   0
	:file   "a.txt" 12
	none    "plain"  3
] 2
--test-- "a list-view's text skips the icon" [is? icon-view/text "a.txt"]

;; `capture` - a window's client area or a widget, as an image! of device
;; pixels - within a pixel of the logical size times the scale.
about?: func [size [pair!] want [pair!] /local d][
	d: size - want
	any [all [(abs d/x) <= 1  (abs d/y) <= 1]  ajoin ["got " size " for " want]]
]
shot: capture trees
--test-- "capture of a window" [all [image? shot  about? shot/size trees/size * trees/scale]]
shot: capture icon-view
--test-- "capture of a widget" [about? shot/size icon-view/size * trees/scale]
shot: none

--test-- "tree-view kind, nothing picked" [all [tree/kind = 'tree-view  none? tree/selected]]
tree/selected: 'docs/old/"b.txt"
--test-- "picked by path" [is? mold tree/selected {docs/old/"b.txt"}]
--test-- "its text is the label" [is? tree/text "b.txt"]
;; The text is the picked node's label, so it is read-only - `selected`
;; picks the node. (Setting it used to reach the native control: an
;; exception on macOS, a renamed window on Windows.)
--rejects-- "a tree-view's text" [tree/text: "x"]
--test-- "and the label is unchanged" [is? tree/text "b.txt"]
--test-- "a path! it is" [path? tree/selected]
--test-- "its branches opened" [is? mold tree/expanded "[docs docs/old]"]
tree/selected: 'report
--test-- "picked by word" [is? mold tree/selected "docs/report"]
tree/selected: "Song.mp3"
--test-- "picked by label" [is? mold tree/selected {music/"Song.mp3"}]
tree/selected: 'music
--test-- "a node at the top is a word" [all [word? tree/selected  tree/selected = 'music]]
tree/selected: "Notes.txt"
--test-- "... or a string without a word" [is? tree/selected "Notes.txt"]
tree/selected: 'docs/nothing
tree/selected: [music "Song.mp3"]
--test-- "a block works too" [is? mold tree/selected {music/"Song.mp3"}]
tree/selected: 'docs/nothing
--test-- "a path to nothing picks nothing" [none? tree/selected]
tree/expanded: [music docs/old]
--test-- "opened as asked, with what is above" [is? mold tree/expanded "[docs docs/old music]"]
tree/expanded: [docs/old]
--test-- "exactly those, and above" [is? mold tree/expanded "[docs docs/old]"]
--test-- "every node" [is? length? tree/nodes 8]
--test-- "the fifth node" [is? mold pick tree/nodes 5 {docs/old/"b.txt"}]
--test-- "the same items block back" [same? tree/items tree/items]
;; Setting `items` again starts afresh: nothing picked, nothing open.
;;
;; WATCH: the tree shows Documents open and Old inside it CLOSED. On macOS
;; the outline used to keep the old tree's branches open - and to ask for
;; rows of the nodes just freed.
tree/selected: 'report
tree/items: tree/items
--test-- "items set again: nothing picked, nothing open" [all [none? tree/selected  empty? tree/expanded]]
tree/expanded: [docs]
--test-- "one branch opened" [is? mold tree/expanded "[docs]"]
spare-tree: add-tree-view trees ["x"] 0x0 0x0
--test-- "a zero size measures a tree" [all [spare-tree/size/x > 0  spare-tree/size/y > 0]]
remove-widget spare-tree

;; `row-height` is the height of a row of any of the three kinds with rows,
;; in logical units; none hands it back to the platform, which follows the
;; font.
own-height: tree/row-height
--test-- "the platform's row height" [all [integer? own-height  own-height > 0]]
tree/row-height: 32
tree/font-size: 14
--test-- "row-height set, and kept past a new font" [is? tree/row-height 32]
tree/row-height: none
tree/font-size: none
--test-- "none puts the platform's back" [is? tree/row-height own-height]
--test-- "no row-height on other kinds" [none? counter/row-height]

;;=============================================================================
group "Menu bar"
;;=============================================================================

;; A label followed by a WORD is an item - and it is the word, not the label,
;; which comes back in the event, so renaming "Reset" would not break the
;; handler below. A label followed by a BLOCK is a submenu. `---` divides.
;;
;; A char! after the id is a shortcut on the platform's own menu modifier -
;; Ctrl on Windows, Cmd on macOS - and a block adds more modifiers to it.
menu-spec: [
	"File" [
		"Reset counter" reset  #"R"
		"Log a note"    note   [shift #"L"]
		---
		"Close"         quit   #"W"
	]
	"View" [
		"Image" [
			"Repaint"   repaint
			"Clear log" clear-log
		]
		---
		"Big text"      big
		"Normal text"   normal
		---
		"Track the mouse outside" track
		"Dark controls"           dark-controls
	]
	"Help" ["About" about]
]
win/menu: menu-spec

;; Reads back as the very block it was given - not a rebuilt one.
--test-- "the menu is the very block given" [same? win/menu menu-spec]

;; Greying out is by word, and merges: only `note` changes here.
win/menu-enabled?: [note false]
enabled: win/menu-enabled?
--test-- "menu-enabled? greys one out"  [false = select enabled 'note]
--test-- "... and leaves the rest alone" [all [true = select enabled 'reset  true = select enabled 'about]]

;;=============================================================================
group "Dropped files"
;;=============================================================================

;; Off until asked for: a window which silently swallows a drop is worse than
;; one which visibly refuses it.
--test-- "drop? off by default" [not win/drop?]
win/drop?: true
--test-- "drop? on" [win/drop?]


;; TRY IT: drag files onto the window. The log names them and says what they
;; landed on - drop one onto the picture and it is loaded into the canvas.
;; Dropping TEXT works on macOS, where the pasteboard gives it for nothing;
;; on Windows it needs a registered IDropTarget and is not implemented.
print {
Drag a file onto the window - the log names it and what it landed on.
Drop an image onto the picture and it replaces it.
}

;;=============================================================================
group "Events"
;;=============================================================================

print {
Move the mouse over the window, click, use the wheel, resize it.
TAB and Shift-TAB move between controls - including into the panel.
TAB out of an area moves on WITHOUT typing a tab into it - try the flat
area in the "Fixed size" window: its text must not change.
TAB and Shift-TAB also leave the drop-down's typed text.
CTRL+R, CTRL+W and SHIFT+CTRL+L are the menu shortcuts.
The arrows move within a radio group; SPACE presses what is focused.
Paste (Ctrl+V) into the field puts the text in ONCE; typed keys come once too.
On macOS too, TAB and Shift-TAB move between the fields, areas and lists;
buttons, checks and radios join in only with Keyboard navigation on in the
system settings, as in any Mac app.
On macOS: Cmd+X, Cmd+C, Cmd+V, Cmd+A and Cmd+Z / Shift+Cmd+Z work in the
fields and in the areas - copying out of the read-only log too.
ENTER in the field reports a click; ESCAPE is deliberately ignored.
"Click me" counts clicks and repaints the image.
Right-click the image for its context menu.
The checkbox enables and disables it; the radios come in two groups.
Dragging the slider drives the progress bar below it.
Picking from the drop-list shows up in the label; the drop-down lets you
pick OR type your own text.
Typing in the field greets you in the label above it.
Clicks, edits and focus changes are logged into the area.
"Close it" - or the title bar - ends the test.
Events over the image report the image widget as their source.
}

;; `move` events are collapsed by the extension, but there are still plenty
;; of them - only report a move once it has travelled a bit.
last-move: 0x0
pressed:   none   ;; the control between its `down` and its `up`
float-grab: none  ;; where the `floating` button was grabbed
held:      false  ;; the left button is down in a window
last-scale: win/scale  ;; the main window's scale, to notice it change
tracking:   false      ;; `track-mouse`, toggled from the View menu
hovered:    none       ;; what the last `enter` was for, until its `leave`
clicks: 0

;; ONE argument now: `poll-events` returns a block of event! values, so the
;; handler reads what it needs by name instead of counting positions. `window`
;; is the window for window events and the WIDGET itself for a click, a change
;; or a focus change.
report: func [event /local type source position kind col dir][
	type:     event/type
	source:   event/source
	position: event/offset
	;; Windows and drops have no `kind`, so it is only asked of a widget.
	kind:     all [source/type = 'GUI-WIDGET  source/kind]

	;if all [type = 'move  10 > distance last-move position] [exit]
	if type = 'move [last-move: position]

	;; Every mouse event is in the client coordinates of its window, whatever
	;; its source. With no button held, a move comes from the window under
	;; the pointer - including over its widgets, which report it with
	;; themselves as the source - so it always lands inside that window,
	;; and inside the widget it names. (A drag belongs to where it started
	;; and may leave both.)
	switch type [down [held: true] up [held: false]]
	if all [type = 'move  not held] [
		case [
			source/type = 'GUI-SCREEN [
				;; `track-mouse` (View menu): a move outside every window
				;; of ours, measured from that screen's own corner.
				unless within? position 0x0 source/size [
					note ajoin ["!! move outside its screen: " position]
				]
			]
			source/type = 'GUI-WINDOW [
				unless within? position 0x0 source/size [
					note ajoin ["!! move outside its window: " position]
				]
			]
			true [
			if kind = 'text [note "!! move from a label - labels are see-through"]
			unless within? position source/at source/size [
				note ajoin ["!! move on " kind " not over it: " position
				            " (it is at " source/at ")"]
			]
		]]
	]

	;; The scroller fills its window - see the Scroller section.
	if all [type = 'resize  source == viewer] [fit-viewer]

	;; WATCH (Windows, two monitors with different scale settings): drag the
	;; main window from one to the other. It must arrive the same size in
	;; logical units - the `resize` it reports keeps the numbers - with every
	;; widget in place and its text at the new monitor's sharpness, and
	;; `scale` changing to the new monitor's.
	if all [type = 'resize  source == win] [
		unless last-scale = win/scale [
			if last-scale [
				note ajoin ["scale " last-scale " -> " win/scale " on " win/screen/name
				            " - client size still " position]
			]
			last-scale: win/scale
		]
	]

	;; `enter` and `leave`: what the pointer is over. Every `enter` is for
	;; something new, every `leave` for what was entered last, and every
	;; `move` comes from what the pointer is over - so they pair up, and
	;; the order is leave, enter, move.
	;;
	;; WATCH: the title bar names what the pointer is over - the widget, or
	;; the window's background - and goes back to plain when it leaves the
	;; window. That is all a tooltip needs to know.
	switch type [
		enter [
			if hovered [note ajoin ["!! enter while still over " any [attempt [hovered/kind] hovered/type]]]
			hovered: source
			;; A screen (with `track-mouse`) belongs to no window.
			if win == case [
				source/type = 'GUI-WIDGET [source/window]
				source/type = 'GUI-WINDOW [source]
			][
				win/title: ajoin ["Rebol GUI extension - over " any [kind "the window"]]
			]
		]
		leave [
			;; `hovered` starts as none: the `enter` for whatever the
			;; pointer was over when the window opened went out with the
			;; events the sections above drained with `poll-events`.
			if all [hovered  not hovered == source] [note "!! leave for something not entered"]
			hovered: none
			if source == win [win/title: "Rebol GUI extension"]
		]
		move [
			if all [hovered  not hovered == source] [note ajoin ["!! move from " any [kind source/type] " before its enter"]]
		]
	]

	;; Everything a control reports about itself goes into the area, which
	;; is also how the area proves it can be written to while in use.
	;; A continuous slider fires a `change` for every step of the drag, so
	;; it is the one thing not written into the log.
	if all [
		find [click change focus unfocus] type
		not all [type = 'change  kind = 'slider]
	][	note ajoin [type " on " kind] ]

	;; `down` and `up` on the pressable controls: every `down` must be
	;; answered by exactly one `up` on the same control, with `move` and a
	;; slider's `change` events in between and a `click` after.
	;;
	;; WATCH (macOS especially): `down` must appear in the log as soon as
	;; the button is pressed, not when it is released.
	if all [
		find [down up] type
		find [button check radio slider] kind
	][
		note ajoin [type " on " kind " at " position]
		either type = 'down [
			if pressed [note "!! down while another press is still open"]
			pressed: source
		][
			unless pressed == source [note "!! up without a matching down"]
			pressed: none
		]
	]
	if all [type = 'click  pressed == source] [
		note "!! click arrived before up"
	]
	;; While a control is pressed, every move is its own.
	if all [type = 'move  pressed  not pressed == source] [
		note ajoin ["!! move from " any [kind "the window"] " during a press"]
	]

	;; WATCH: the `floating` button drags its borderless window around.
	;; The offset is in the window's client coordinates, which move with the
	;; window, so the grab point stays under the pointer.
	if source == float [
		switch type [
			down [float-grab: position]
			move [if float-grab [ghost/offset: ghost/offset + position - float-grab]]
			up   [float-grab: none]
		]
	]

	;; WATCH: a right click on the picture opens a context menu; what is picked
;; is logged, and dismissing it logs `none`. The window keeps reporting while
;; the menu is up - its events arrive at the next poll.
if all [type = 'alt-down  source == canvas] [
	picked: popup-menu canvas [
		"Repaint"     repaint  #"P"
		"Clear log"   clear-log
		---
		"More" ["Say hello" hello  "Say goodbye" goodbye]
	]
	note ajoin ["context menu: " mold picked]
	switch picked [
		repaint   [paint pic random 400  redraw canvas  recolor-icon]
		clear-log [logged: copy ""  log/text: ""]
	]
]

;; Key downs only - the ups would double the log.
	if find [key named-key] type [
		note ajoin [type ": " mold event/key
			either event/flags [join " " mold event/flags][""]
			" in " any [kind 'window]
			either caps-lock? [" (caps lock)"][""]]
	]

	if type == 'change [
		case [
			source == picker [
				;; Picking from the drop-list shows up in the label.
				label/text: ajoin ["Picked: " source/text " (" source/index ")"]
			]
			source == combo [
				;; Picking OR typing in the drop-down shows up in the label -
				;; no index shown, since typed text may not match any item.
				label/text: ajoin ["Combo: " source/text]
			]
			source == when [
				;; A new date or time, picked by the user.
				note ajoin ["date-field: " source/value]
			]
			source == files [
				note ajoin ["list-view row: " source/index " " mold source/text]
			]
			source == trees [
				;; ... and so does picking from the text-list.
				label/text: ajoin ["Tree: " source/text " (" source/index ")"]
			]
			source == level [
				;; Dragging the slider drives the progress bar next to it.
				meter/value: source/value
			]
			source == name [
				;; The field greets whoever is typing in it.
				label/text: either empty? source/text [
					"Type your name:"
				][	ajoin ["Hello, " source/text "!"] ]
			]
			source == riser [
				log/scroll: 100% - riser/value
			]
		]
	]

	;; A drop's source is a handle of its own, which carries the content and
	;; what it was dropped ON - so a handler does not have to remember what
	;; the pointer was over. `count` saves walking the block to ask.
	if find [drop-file drop-text] type [
		note ajoin [
			type " on " either source/target = win ["the window"][source/target/kind]
			" (" source/count ")"
		]
		either type = 'drop-file [
			;; A block of file!, already in Rebol path form.
			foreach file source/data [note ajoin ["  " file]]
			;; Dropping an image onto the canvas shows it. `pic` is NOT
			;; reassigned: it stays the small gradient buffer the Click
			;; handler paints into, because that handler walks every pixel
			;; in interpreted Rebol and a dropped photo can be tens of
			;; megapixels. Nothing pumps the OS queue while Rebol is inside
			;; a loop, so painting one would freeze the window for as long
			;; as it took - the widget only ever held a REFERENCE, so
			;; showing one image and painting another is free.
			if all [source/target = canvas  attempt [dropped: load first source/data]] [
				if image? dropped [
					note ajoin ["  showing it (" dropped/size ")"]
					try [dropped: resize dropped pic/size * win/scale]
					canvas/image: dropped
					redraw canvas
				]
			]
		][
			;; One string, however many lines it has.
			name/text: source/data
		]
	]

	;; WATCH: switch the system between light and dark. The log names the new
	;; appearance, once per switch - Windows broadcasts the setting several
	;; times, and only a real change is reported. On macOS the controls
	;; follow by themselves; on Windows only the title bar does.
	if type == 'theme-change [
		note ajoin ["theme: " event/code " (dark? " source/dark? ")"]
		;; The one colour of our own in this window follows the switch.
		if source == win [box/background: panel-color did all [win/dark-controls? source/dark?]]
		if source == viewer [color-viewer]
		unless (event/code = 'dark) = source/dark? [note "!! code and dark? disagree"]
	]

	;; A menu pick carries the item's WORD in `code` - the core reads a symbol
	;; id back as a word, which is what keeps this a plain switch.
	if type == 'menu-select [
		note ajoin ["menu: " event/code]
		switch event/code [
			reset     [clicks: 0  counter/text: "Click me"]
			note      [note "a note"]
			;; WATCH: the dialog blocks the main window until answered;
			;; the window's own close box can't close it meanwhile.
			quit      [if confirm-quit [close-window win  exit]]
			repaint   [paint pic random 400  redraw canvas  recolor-icon]
			clear-log [logged: copy ""  log/text: ""]
			;; Re-fit after the font change, which is what makes the
			;; label grow down over the field's top edge and back - the
			;; interactive version of the repaint check above.
			big       [label/font-size: 20   label/size: 220x0]
			;; WATCH: with this on, moving the mouse OUTSIDE every window
			;; logs `move` events whose source is a screen; back over a
			;; window they come from the window again, never both.
			dark-controls [
				win/dark-controls?: not win/dark-controls?
				box/background: panel-color did all [win/dark-controls? win/dark?]
				;; The table window follows, and its stripe with it.
				if table/open? [
					table/dark-controls?: win/dark-controls?
					files/background: stripes did all [table/dark-controls? table/dark?]
				]
				note ajoin ["dark controls: " win/dark-controls?]
			]
			track     [
				tracking: not tracking
				track-mouse tracking
				note ajoin ["tracking the mouse outside: " tracking]
			]
			normal    [label/font-size: none label/size: 220x0]
			about     [label/text: "Rebol/GUI extension"]
		]
		;; Once the counter is back at zero there is nothing to reset.
		win/menu-enabled?: reduce ['reset clicks > 0  'note true]
	]

	if all [type == 'close source/type = 'GUI-WINDOW] [
		print ["Closing window:" source]
		close-window source
	]

	;; A header of the list-view was clicked: `code` is the column. Sorting is
	;; ours - the same column again flips the direction.
	if type == 'sort [
		col: event/code
		dir: either col = source/sort-column [negate col][col]
		either dir > 0 [
			sort/skip/compare source/items 3 col
		][	sort/skip/compare/reverse source/items 3 col ]
		source/items: source/items
		source/sort-column: dir
		note ajoin ["sort: column " col either dir > 0 [" up"][" down"]]
	]

	;; For a click the source is the widget, not the window.
	if type == 'click [
		case [
			source == files [note ajoin ["list-view activated: " mold source/text]]
			source == closer [
				close-window win
				exit
			]
			;; ENTER in a date-field too.
			source == when [note ajoin ["date-field entered: " source/value]]
			;; ENTER in a field reports a `click` - the same word a button
			;; uses, because it is the same thing: the control was activated
			;; rather than merely edited. An area keeps Enter for itself.
			source == name [
				note ajoin ["entered: " mold source/text]
				label/text: either empty? source/text [
					"Type your name:"
				][	ajoin ["Hello, " source/text "!"] ]
			]
			;; A check has already toggled itself by the time this arrives.
			source/kind = 'check [
				counter/enabled?: source/state
				note ajoin ["counting " either source/state ["on"]["off"]]
			]
			;; A toggle, like a check, reports the state it settled into.
			source == bolder [
				log/bold?: source/state
				note ajoin ["log bold: " source/state]
			]
			;; Toggle keys listening.
			source == keys-switch [
				win/keys?: keys-switch/state
				note ajoin ["catch keys: " win/keys?]
			]
			;; ... and a radio has already settled its group.
			source/kind = 'radio [
				note ajoin ["group " source/group " -> " source/text]
			]
			source == counter [
				clicks: clicks + 1
				source/text: ajoin ["Clicked " clicks]

				;; Drawing into the very same image the widget was given,
				;; then asking for it to be shown. Nothing is copied.
				paint pic clicks * 40
				unless same? canvas/image pic [canvas/image: pic]
				redraw canvas
			]
			;; The borderless window's own button gives it a frame back - and with
			;; a title bar it has a close box again - and takes it away again
			;; on the next press. The label says which comes next, and the
			;; zero size re-fits the button to it.
			source == bordered [bare/border?: source/state]
			source == back-again [
				either bare/title? [
					bare/title?: false
					back-again/text: "Give me a frame"
				][
					bare/title?: true
					bare/title: "There you go"
					back-again/text: "Hide the title"
				]
				back-again/size: 0x0
			]
		]
	]

	print [
		as-green pad form type 10
		pad mold position 12
		case [
			;; A wheel event is `scroll-line`, and its line count is in `code` -
			;; where a position would otherwise be, which is why it has none.
			type = 'scroll-line [ajoin ["lines: " event/code]]
			type = 'click [source]

			find [open close] type [
				ajoin ["item:" event/code " " source]
			]
			;; The image widget reports its own mouse events, in window
			;; coordinates like everything else - `at` turns them into
			;; a position on the picture.
			source == canvas [ajoin ["on the image at " position - canvas/at]]
			source/type = 'GUI-SCREEN [ajoin ["on screen " mold source/name " at " position]]
			type = 'menu-select [ajoin ["item: " event/code]]
			;; the drop handle molds as what it holds
			find [drop-file drop-text] type [ajoin ["dropped " mold source]]
			;; shift / control / alt / double, already as words
			true [mold event/flags]
		]
	]
]


;;- modal dialogs ------------------------------------------------------------
;; `/modal owner` blocks every other window until the dialog closes; the owner
;; only decides placement (centred on it). Dialogs nest - only the newest one
;; takes input.
dlg: open-window/modal/title 260x100 win "Confirm"
add-text dlg "A modal dialog" 10x10 0x0
--test-- "a dialog is modal?"    [dlg/modal?]
--test-- "its owner is not"      [not win/modal?]
--rejects-- "closing the owner of an open dialog" [close-window win]
inner: open-window/modal/title 200x80 dlg "Nested"
--rejects-- "closing a nested owner" [close-window dlg]
close-window inner
close-window dlg
--test-- "after the dialogs, the main window is still open" [win/open?]
;; WATCH: while a dialog was up nothing else took clicks or keys; a
;; theme-change still arrives from blocked windows.

;; release on an owner cannot be refused - it closes its dialogs first.
spare: open-window/title 200x80 "Spare owner"
dlg2: open-window/modal 160x60 spare
release spare
--test-- "a dialog closes with its released owner" [not dlg2/open?]

;; A modal Yes/No dialog run by a nested `do-events` - returns true for Yes.
confirm-quit: function [][
	answer: false
	dlg: open-window/modal/title 240x100 win "Quit?"
	add-text dlg "Close the test window?" 20x16 0x0
	yes: add-button dlg "Yes" 20x52 90x30
	no:  add-button dlg "No" 130x52 90x30
	do-events dlg func [event][
		if event/type == 'click [
			answer: event/source == yes
			close-window dlg
		]
	]
	release dlg
	answer
]

;; The checks so far, before the windows are handed to the user; the totals
;; are printed again, with what follows, once the main window is closed.
print ajoin [lf as-yellow "so far: " test-count " tests, " fail-count " failed"]

;; Pumps the OS queue, hands every event! to `report`, and returns once the
;; window has been closed. This is the whole event model - there is no
;; system/ports/event involvement at all.
do-events win :report
track-mouse false   ;; nothing left to report to

;;=============================================================================
group "After closing"
;;=============================================================================

--test-- "the window is closed" [not win/open?]
--test-- "a widget of a closed window reads none" [none? attempt [name/text]]

;; Widgets go with their window: the handles stay usable and simply report
;; themselves as removed, rather than pointing at freed controls.
--test-- "a button has no parent after its window closed" [none? counter/parent]
--test-- "its id is 0" [is? counter/id 0]

;; Everything the panel held went with it, however it was reached.
--test-- "nor a radio inside the panel" [none? warm/parent]

;; The extra windows go too - a `close` on the main one ends the loop, and
;; these two have nothing watching them.
foreach extra reduce [fixed bare tinted ghost tabbed table viewer trees] [
	if extra/open? [close-window extra]
]

;; The image itself is untouched by any of this - the widget only ever held
;; a reference to it.
--test-- "the image survives" [all [image? pic  pic/size = 240x160]]

;; The handles stay usable after the window is gone. Releasing them is
;; optional - the recycler would do it too.
foreach handle reduce [
	canvas counter closer label name log styled
	toggle box warm cool slow fast level meter picker combo rule hr tabs keys-switch files
	fixed bare back-again bordered
][	release handle ]
;; Screen handles lock nothing native, so they need no release - the
;; collector takes them once nothing refers to them.
release win
print ["released:" win]

summary
