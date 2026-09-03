REBOL [
	Title:   "Rebol GUI extension"
	Name:    gui
	Version: 0.1.0
	Needs:   3.22.5
	Author:  @Oldes
	License: Apache-2.0
	Options: [delay]
	Exports: [
		open-window close-window show-window hide-window
		add-button add-image add-text add-field add-area
		add-check add-radio add-slider add-progress add-drop-down add-panel
		remove-widget redraw
		poll-events do-events event-flags
	]
	Purpose: {
		A minimal, GOB-free windowing extension.

		This first version does exactly one thing: it opens native windows
		of a requested client size and title, and hands mouse events back
		to Rebol as plain values. There is no compositor, no DRAW dialect
		and no dependency on the host's View sources - drawing is expected
		to arrive later as an image! blitted into the window.

		Events are NOT posted to system/ports/event. The extension keeps
		its own queue and `poll-events` drains it, which means the whole
		thing works against an unmodified interpreter: no device
		registration, no host-lib callbacks, no REBGOB crossing the
		extension boundary.
	}
]

;; ---------------------------------------------------------------------------
;; C-side configuration
c-prefix: GUI

;; Extra declarations the generated header must carry.
;;
;; The payloads are deliberately opaque here: `handle` is a void* rather than
;; an HWND or an NSView*, so that gen-gui.h stays free of windows.h and of
;; AppKit, and the command sources can be compiled without either.
;;
;; Widgets are kept in an intrusive list on their window. That list is what
;; lets a closing window take its children down with it - the OS destroys the
;; native controls, and this is how the Rebol handles hear about it.
c-header: {
extern REBCNT Handle_GuiWindow;
extern REBCNT Handle_GuiWidget;

typedef struct Gui_Window_Context {
	void   *handle;  // native window (HWND / NSWindow*)
	REBHOB *hob;     // back reference, so the window proc can tag its events
	REBCNT  flags;   // GUIW_* bits
	void   *widgets; // head of the child widget list (GUIWIDGET*)
} GUIWIN;

// An image widget holds no pixels of its own: the image! series it was given
// is kept in `hob->series`, which the GC marks, and the backend reads the
// dimensions and the data from it at every paint. So drawing into that same
// image from Rebol - or from another extension - shows up on the next redraw,
// with nothing copied in between.
typedef struct Gui_Widget_Context {
	void   *handle;  // native control (HWND / NSView*)
	REBHOB *hob;     // back reference, as above; hob->series is the image!
	REBCNT  kind;    // W_GUI_WIDGET_* - what the control is
	GUIWIN *owner;   // WINDOW it ends up in, however deeply nested; NULL once
	                 // that window is gone
	void   *parent;  // containing panel (GUIWIDGET*), NULL when the window
	                 // holds it directly
	void   *next;    // next widget of the same window (GUIWIDGET*) - the list
	                 // is FLAT and window-wide, whatever the nesting, so one
	                 // walk still reaches every widget at teardown
	REBCNT  group;   // radio group id; 0 for every other kind
	REBCNT  state;   // check / radio: 1 when on. The extension is the source
	                 // of truth here, not the native control - see the radio
	                 // grouping note in gui-commands.c
} GUIWIDGET;

#define GUIW_VISIBLE  1
}

;; ---------------------------------------------------------------------------
;; Words resolved at init time through RL_MAP_WORDS.
;;
;; ORDER IS SIGNIFICANT: the generated W_GUI_EVENT_* enum starts at 1 (after
;; the _0 sentinel) and `Gui_event_words` is the very same 1-based array, so
;; the C side can emit an event word with a plain `Gui_event_words[type]`.
;;
;; The `arg:` list is not written here - the generator collects it from the
;; `handles:` field below.
;; NOTE: append to these lists, never insert - the enum values are positions.
words: [
	event: [
		move            ;; mouse moved over the client area
		down up         ;; left button
		alt-down alt-up ;; right button
		aux-down aux-up ;; middle button
		wheel           ;; value = signed number of lines
		close           ;; the user asked to close it; the window is still open
		resize          ;; position = the new client size
		click           ;; a widget was activated; source = the widget
		change          ;; the user edited a field or an area
		focus unfocus   ;; keyboard focus entered or left a widget
	]
	widget: [
		button
		image
		text            ;; a static label
		field           ;; one line of editable text
		area            ;; several lines of editable text, with a scrollbar
		check           ;; a checkbox, toggled on its own
		radio           ;; one of a group; see `group` below
		slider          ;; draggable, reports `change`
		progress        ;; shows a value, takes no input
		drop-down       ;; pick one of a list; reports `change`
		panel           ;; holds other widgets; see `parent` below
	]
]

;; ---------------------------------------------------------------------------
;; Handle types and their path accessors.
handles: [
	window: [
		"GUI window handle"
		;NAME    GET       SET       DESCRIPTION
		title    string!   string!   "Text shown in the title bar"
		size     pair!     pair!     "Size of the client area in pixels"
		offset   pair!     pair!     "Position of the top-left corner on the screen"
		id       integer!  none      "Native window handle as an integer"
		open?    logic!    none      "False once the window has been closed"
	]
	widget: [
		"GUI widget handle - a native control inside a window"
		;NAME    GET       SET       DESCRIPTION
		text     string!   string!   "Label or contents; the selected item of a drop-down, which is read-only; none for an image"
		items    block!    block!    "Strings a drop-down offers; none for other kinds"
		index    integer!  integer!  "Which item is picked, 1-based; 0 for none"
		image    image!    image!    "Image shown by an image widget, none for other kinds"
		size     pair!     pair!     "Size of the control"
		offset   pair!     pair!     "Position inside whatever holds it - a window or a panel"
		id       integer!  none      "Native control handle as an integer"
		kind     word!     none      "What the control is: button, image, text, field, area, check, radio, slider, progress or drop-down"
		value    percent!  [percent! decimal!] "Position of a slider or a progress bar; none for other kinds"
		state    logic!    logic!    "Whether a check or a radio is on; none for other kinds"
		group    integer!  none      "Which radio group it belongs to; 0 for everything else"
		enabled? logic!    logic!    "Whether the control responds to the user"
		parent   handle!   none      "Whatever holds it - a window, or a panel; none once gone"
		window   handle!   none      "The window it ends up in, however deeply nested"
	]
]

;; ---------------------------------------------------------------------------
;; Commands. Order is significant - it fixes the command indices, so new ones
;; are appended rather than inserted.
commands: [
	open-window: [
		"Creates a window and returns its handle"
		size [pair!] "Size of the client area"
		/title text [string!] "Text shown in the title bar"
		/at offset [pair!] "Position of the top-left corner on the screen"
		/hidden "Creates the window without showing it"
	]
	close-window: ["Destroys the window" window [handle!]]
	show-window:  ["Makes the window visible" window [handle!]]
	hide-window:  ["Hides the window without destroying it" window [handle!]]
	poll-events:  ["Dispatches pending OS messages and returns the collected events"]
	add-button: [
		"Creates a native push button inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		text   [string!] "Label"
		offset [pair!]   "Position inside the client area"
		size   [pair!]   "Size of the button"
	]
	remove-widget: ["Destroys a widget" widget [handle!]]
	add-image: [
		"Creates an image widget inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		image  [image!]  "Shown as is; the widget keeps a reference, not a copy"
		offset [pair!]   "Position inside the client area"
		/size sz [pair!] "Scales the image to this size (default: the image's own)"
	]
	redraw: [
		"Repaints a window or a widget - use after drawing into a displayed image"
		target [handle!]
	]
	add-text: [
		"Creates a static label inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		text   [string!]
		offset [pair!]   "Position inside the client area"
		size   [pair!]
	]
	add-field: [
		"Creates a one-line text entry inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		text   [string!] "Initial contents"
		offset [pair!]   "Position inside the client area"
		size   [pair!]
	]
	add-area: [
		"Creates a multi-line text entry inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		text   [string!] "Initial contents"
		offset [pair!]   "Position inside the client area"
		size   [pair!]
	]
	add-check: [
		"Creates a checkbox inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		text   [string!] "Label"
		offset [pair!]   "Position inside the client area"
		size   [pair!]
	]
	add-radio: [
		"Creates a radio button inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		text   [string!] "Label"
		offset [pair!]   "Position inside the client area"
		size   [pair!]
		/group id [integer!] {Radios sharing an id turn each other off (default: 0)}
	]
	add-slider: [
		"Creates a slider inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		offset [pair!] "Position inside the client area"
		size   [pair!] "Taller than wide makes it vertical"
		/value val [percent! decimal!] "Initial position (default: 0%)"
	]
	add-progress: [
		"Creates a progress bar inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		offset [pair!] "Position inside the client area"
		size   [pair!]
		/value val [percent! decimal!] "Initial position (default: 0%)"
	]
	add-drop-down: [
		"Creates a drop-down list inside a window and returns its handle"
		parent [handle!] "Window or panel to put it in"
		items  [block!] "Strings to offer"
		offset [pair!]  "Position inside the client area"
		size   [pair!]  "Of the closed control; room for the list is added"
		/index n [integer!] "Item picked to start with, 1-based (default: none)"
	]
	add-panel: [
		"Creates a panel - a widget which holds other widgets - and returns its handle"
		parent [handle!] "Window or panel to put it in"
		offset [pair!]   "Position inside the client area"
		size   [pair!]
	]
]

;; ---------------------------------------------------------------------------
;; Module body.
mezzanine: [
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
]