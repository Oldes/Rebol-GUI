Rebol [
	Title:   "Rebol/GUI extension test"
	Needs:   3.22.5
	Purpose: {
		Opens a window and prints the mouse events it produces. Meant to be
		run by a human - close the window to end it.

		Usage:
			r3 test.r3
	}
]

print ["Running test on Rebol build:" mold to-block system/build]

;; make sure that we load a fresh extension
try [system/modules/gui: none]

;; a locally built library can be pointed at without installing it
if modules-dir: get-env 'REBOL_MODULES_DIR [
	system/options/modules: dirize to-rebol-file modules-dir
]

gui: import 'gui
? gui

NL: either system/platform = 'Windows [CRLF][LF]

;;=============================================================================
print as-yellow "^/== Opening a window"
;;=============================================================================

win: open-window/title/at 640x480 "Rebol GUI extension" 200x120

print ["window:  " win]
print ["id:      " win/id]
print ["open?:   " win/open?]
print ["title:   " mold win/title]
print ["size:    " win/size]
print ["offset:  " win/offset]
print ["scale:   " win/scale "(device pixels per unit - sizes below are logical)"]
print ["resizable?" win/resizable? " border?" win/border?]

;; every accessor which can be read can also be written, except id and open?
win/title: "Rebol GUI extension - move the mouse"
print ["title:   " mold win/title]

;;=============================================================================
print as-yellow "^/== Native buttons"
;;=============================================================================

;; Both are children of the window's client area, positioned from its
;; top-left corner - the same convention everything else here uses.
;; A zero axis asks the control what it needs for its own text, in its
;; own font - which is the only thing that knows. Everything else here is
;; in LOGICAL units: 96 to the inch, the same number on any display.
counter: add-button win "Click me"  20x20 140x32
closer:  add-button win "Close it" 180x20 0x0

print ["button:   " counter]
print ["kind:     " counter/kind]
print ["text:     " mold counter/text]
print ["offset:   " counter/offset]
print ["size:     " counter/size]
print ["enabled?: " counter/enabled?]
print ["parent:   " counter/parent "^/is the window?" counter/parent = win]

;;=============================================================================
print as-yellow "^/== An image widget"
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

print ["image:    " canvas]
print ["kind:     " canvas/kind]
print ["size:     " canvas/size]
;; Reading the image back out of the widget. NEVER mold an image! into the
;; console - that is every pixel as text - so only its type and size.
;;
;; The size is worth printing: it comes from the series itself, so a wrong
;; answer here would mean the accessor lost the dimensions on the way out.
shown: canvas/image
print ["image is: " type? shown "size:" shown/size "(expected 240x160)"]
print ["same series:" same? shown pic]
shown: none
print ["text:     " mold canvas/text  "(none - an image carries no text)"]

;;=============================================================================
print as-yellow "^/== Text widgets"
;;=============================================================================

;; A label, a one-line entry and a multi-line entry. All three carry their
;; string in the same `text` accessor the button uses.
label: add-text  win "Type your name:"  300x70  220x0
name:  add-field win ""                 300x100 240x0
log:   add-area  win "-- event log --"  300x140 300x200

print ["label kind:" label/kind "  field kind:" name/kind "  area kind:" log/kind]
print ["natural heights - label:" label/size "field:" name/size]
print ["and the button that asked for one:" closer/size]
print ["label text:" mold label/text]

;; Writing to a control from Rebol does NOT come back as a `change` event -
;; only what the user types does.
name/text: "world"
print ["field text after setting it:" mold name/text]

;;=============================================================================
print as-yellow "^/== Typography"
;;=============================================================================

;; Anything with `text` has `font`, `font-size`, `bold?`, `italic?` and
;; `color`. The font is read back out of the CONTROL, so what is reported is
;; what is really on screen - including whatever the platform started it with.
print ["label started as:" mold label/font label/font-size "bold?" label/bold?]

label/font-size: 15
label/bold?:     true
label/color:     30.90.170
print ["... and is now:  " mold label/font label/font-size "bold?" label/bold?]
print ["colour reads back as:" mold label/color]

;; A control does NOT resize itself for a bigger font - the box laid out is
;; the box kept, so leave room.
print ["size is unchanged:" label/size]

;; `none` puts a part back to whatever the platform uses.
log/font: "Courier New"          ;; a missing family falls back, never fails
log/font-size: 10
name/color: 150.30.30

;; Windows draws a push button's text itself, in the system colour, and only
;; an owner-drawn button could say otherwise. The value is still kept and
;; still reads back - it just does not show there. On macOS it does.
counter/color: 200.110.0
print ["button colour asked for:" mold counter/color]

;; A window carries a default which widgets pick up AS THEY ARE CREATED.
;; Setting it does not reach back into what is already on screen.
win/font-size: 15
win/italic?:   true
styled: add-text win "made after the window default was set" 300x395 320x24

print ["window default:" win/font-size "italic?" win/italic?]
print ["the new label took it:" styled/font-size "italic?" styled/italic?]
print ["the first one did not: " label/font-size "italic?" label/italic?]

;; Put it back, so the rest of this script builds ordinary widgets.
win/font-size: none
win/italic?:   false

;;=============================================================================
print as-yellow "^/== Checks and radios"
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

;; Two independent groups. Radios turn each other off only within a group,
;; and the grouping is the extension's own - it does not depend on creation
;; order, on WS_GROUP flags, or on what AppKit considers a sibling. Note
;; that it does not depend on the panel either: warm and cool are in the
;; box, slow and fast are not, and the two groups still stand apart.
warm: add-radio/group box "Warm"  10x26 110x22 1
cool: add-radio/group box "Cool" 130x26 110x22 1
slow: add-radio/group win "Slow"  20x350 110x22 2
fast: add-radio/group win "Fast" 150x350 110x22 2

warm/state: true
slow/state: true

print ["check:" toggle/kind "state:" toggle/state]
print ["radio:" warm/kind "group:" warm/group "state:" warm/state]

;; The frame and its caption are readable and writable after the fact - both
;; are drawn by the extension at paint time, so neither rebuilds the control
;; and neither moves anything the panel holds.
print ["panel edge:" box/edge "caption:" mold box/text]
box/text: "Temperature (group box)"
print ["... retitled to:" mold box/text]
print ["a check has no edge:" mold toggle/edge]

;; `parent` is whatever holds it; `window` is the window either way.
print ["warm sits in a" warm/parent/kind "at" warm/offset]
print ["... and its window is the same one:" warm/window = win]
print ["slow sits directly in the window:" slow/parent = win]

;; Turning one on turns its own group off - and leaves the other alone.
cool/state: true
print ["after cool/state: true  -> warm:" warm/state "cool:" cool/state]
print ["the other group is untouched -> slow:" slow/state "fast:" fast/state]
warm/state: true

;;=============================================================================
print as-yellow "^/== Slider and progress"
;;=============================================================================

;; Both carry a `value` from 0% to 100%. A slider taller than it is wide
;; would be vertical; these are horizontal.
level: add-slider/value   win 20x370 240x28 25%
meter: add-progress/value win 20x410 240x10 25%

print ["slider:" level/kind "value:" level/value]
print ["progress:" meter/kind "value:" meter/value]
print ["a progress bar has no enabled state:" mold meter/enabled?]

;;=============================================================================
print as-yellow "^/== Drop-down"
;;=============================================================================

;; The only widget which takes a list. `size` is the closed control - room
;; for the list it drops is the backend's problem, not the caller's.
picker: add-drop-down/index win
	["Bilberry" "Cloudberry" "Lingonberry" "Rowan"] 300x350 200x26 2

print ["drop-down:" picker/kind]
print ["items:    " mold picker/items]
print ["index:    " picker/index "-> text:" mold picker/text]

;; Replacing the list is a plain block assignment; the selection is dropped
;; with the items it referred to.
picker/items: ["Ash" "Birch" "Elm" "Oak" "Rowan"]
picker/index: 4
print ["after replacing the items:" picker/index mold picker/text]

logged: copy "-- event log --"
note: func ["Appends a line to the area" line [string!]][
	logged: log/text
	append logged join NL line
	log/text: logged
]

;;=============================================================================
print as-yellow "^/== Window frames"
;;=============================================================================

;; `/fixed` opens a window the user cannot resize, and `/borderless` one with
;; no title bar and no frame at all. Both are readable and writable afterwards
;; - and changing either keeps the CLIENT size, so nothing inside moves.
fixed: open-window/title/at/fixed 240x120 "Fixed size" 700x120
print ["fixed window - resizable?" fixed/resizable? " border?" fixed/border?]
print ["client size:" fixed/size]

bare: open-window/at/borderless 240x120 700x280
print ["bare window  - resizable?" bare/resizable? " border?" bare/border?]
print ["client size:" bare/size "(unchanged by having no frame)"]

;; A borderless window has no close box and nothing to drag, so the program
;; is the only thing that can move or close it. Give this one a way out.
add-text   bare "No border - and no way to close me" 10x10 220x0
back-again: add-button bare "Give me a frame" 10x50 0x0

;; Turning the border back on does not turn resizing back on: they are two
;; properties, and this window never had the second one.
fixed/resizable?: true
print ["... and now the fixed one is resizable?" fixed/resizable?]

;;=============================================================================
print as-yellow "^/== Menu bar"
;;=============================================================================

;; A label followed by a WORD is an item - and it is the word, not the label,
;; which comes back in the event, so renaming "Reset" would not break the
;; handler below. A label followed by a BLOCK is a submenu. `---` divides.
;;
;; A char! after the id is a shortcut on the platform's own menu modifier -
;; Ctrl on Windows, Cmd on macOS - and a block adds more modifiers to it.
win/menu: [
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
	]
	"Help" ["About" about]
]

;; Reads back as the very block it was given - not a rebuilt one.
print ["menu is a" type? win/menu "of" length? win/menu "menus"]

;; Greying out is by word, and merges: only `note` changes here.
win/menu-enabled?: [note false]
print ["menu-enabled? reports:" mold win/menu-enabled?]

;;=============================================================================
print as-yellow "^/== Events"
;;=============================================================================

print {
Move the mouse over the window, click, use the wheel, resize it.
"Click me" counts clicks and repaints the image.
The checkbox enables and disables it; the radios come in two groups.
Dragging the slider drives the progress bar below it.
Picking from the drop-down shows up in the label.
Typing in the field greets you in the label above it.
Clicks, edits and focus changes are logged into the area.
"Close it" - or the title bar - ends the test.
Events over the image report the image widget as their source.
}

;; `move` events are collapsed by the extension, but there are still plenty
;; of them - only report a move once it has travelled a bit.
last-move: 0x0
clicks: 0

report: func [type source position value][
	if all [type = 'move  10 > distance last-move position] [exit]
	if type = 'move [last-move: position]

	;; Everything a control reports about itself goes into the area, which
	;; is also how the area proves it can be written to while in use.
	;; A continuous slider fires a `change` for every step of the drag, so
	;; it is the one thing not written into the log.
	if all [
		find [click change focus unfocus] type
		not all [type = 'change  source/kind = 'slider]
	][	note ajoin [type " on " source/kind] ]

	;; Picking from the drop-down shows up in the label.
	if all [type = 'change  source = picker] [
		label/text: ajoin ["Picked: " source/text " (" source/index ")"]
	]

	;; Dragging the slider drives the progress bar next to it.
	if all [type = 'change  source = level] [
		meter/value: source/value
	]

	;; The field greets whoever is typing in it.
	if all [type = 'change  source = name] [
		label/text: either empty? source/text [
			"Type your name:"
		][	ajoin ["Hello, " source/text "!"] ]
	]

	;; A `menu` event carries the item's WORD in the value slot - the one
	;; slot with no fixed type - so a handler is a plain switch. `source` is
	;; the window the menu belongs to.
	if type = 'menu [
		note ajoin ["menu: " value]
		switch value [
			reset     [clicks: 0  counter/text: "Click me"]
			note      [note "a note"]
			quit      [close-window win  exit]
			repaint   [paint pic random 400  redraw canvas]
			clear-log [logged: copy ""  log/text: ""]
			big       [label/font-size: 20]
			normal    [label/font-size: none]
			about     [label/text: "Rebol/GUI extension"]
		]
		;; Once the counter is back at zero there is nothing to reset.
		win/menu-enabled?: reduce ['reset clicks > 0  'note true]
	]

	;; The borderless window's own button gives it a frame back - and with
	;; a title bar it has a close box again.
	if all [type = 'click  source = back-again] [
		bare/border?: true
		bare/title: "There you go"
	]

	if all [type = 'close source = fixed] [
		print "Closing fixed window."
		close-window fixed
	]
	if all [type = 'close source = bare] [
		print "Closing bare window."
		close-window bare
	]

	;; For a click the source is the widget, not the window.
	if type = 'click [
		case [
			source = closer [
				close-window win
				exit
			]
			;; A check has already toggled itself by the time this arrives.
			source/kind = 'check [
				counter/enabled?: source/state
				note ajoin ["counting " either source/state ["on"]["off"]]
			]
			;; ... and a radio has already settled its group.
			source/kind = 'radio [
				note ajoin ["group " source/group " -> " source/text]
			]
			source = counter [
				clicks: clicks + 1
				source/text: ajoin ["Clicked " clicks]

				;; Drawing into the very same image the widget was given,
				;; then asking for it to be shown. Nothing is copied.
				paint pic clicks * 40
				redraw canvas
			]
		]
	]

	print [
		as-green pad form type 10
		pad mold position 12
		case [
			type = 'wheel [ajoin ["lines: " value]]
			type = 'click [source]
			;; the image widget reports its own mouse events
			source = canvas [ajoin ["on the image " source]]
			;; a `menu` event carries the item's word in this slot
			type = 'menu [ajoin ["item: " value]]
			true [
				if integer? value [
					mold collect [
						foreach [name bit] event-flags [
							if (value & bit) <> 0 [keep to word! name]
						]
					]
				]
			]
		]
	]
]

;; Pumps the OS queue, hands every event to `report`, and returns once the
;; window has been closed. This is the whole event model - there is no
;; system/ports/event involvement at all.
do-events win :report

print ["^/window closed - open?:" win/open?]
print ["what was typed: " mold attempt [name/text] "(none - the control is gone)"]

;; Widgets go with their window: the handles stay usable and simply report
;; themselves as removed, rather than pointing at freed controls.
print ["button after close:" counter "parent:" counter/parent]

;; Everything the panel held went with it, however it was reached.
print ["a radio inside the panel:" warm "parent:" warm/parent]

;; The extra windows go too - a `close` on the main one ends the loop, and
;; these two have nothing watching them.
foreach extra reduce [fixed bare] [if extra/open? [close-window extra]]

;; The image itself is untouched by any of this - the widget only ever held
;; a reference to it.
print ["the image survives:" type? pic pic/size]

;; The handles stay usable after the window is gone. Releasing them is
;; optional - the recycler would do it too.
foreach handle reduce [
	canvas counter closer label name log styled
	toggle box warm cool slow fast level meter picker
	fixed bare back-again
][	release handle ]
release win
print ["released:" win]

print "done"
