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
extern REBCNT Word_Separator; // the menu dialect's `---`

// How this extension describes a font when it is not inside a control.
//
// The name is a plain UTF-8 C string owned by the struct, NOT a Rebol
// series: a handle context has exactly one GC-marked slot and the image
// widget already uses it, so anything else kept here has to be invisible
// to the collector. Whoever owns a GUIFONT frees its name.
//
// A widget does not carry one of these. Its font lives in the native
// control, which is asked at every read - so a font set by any other means
// is reported honestly, and nothing can drift out of step. Only a WINDOW
// keeps a GUIFONT, because a default has to survive until the next widget
// is created.
typedef struct Gui_Font_Spec {
	char   *name;   // family name; NULL means the platform's own
	REBINT  size;   // in points; 0 means the platform's own
	REBCNT  style;  // GUI_FONT_* bits
} GUIFONT;

#define GUI_FONT_BOLD    1
#define GUI_FONT_ITALIC  2

typedef struct Gui_Window_Context {
	void   *handle;  // native window (HWND / NSWindow*)
	REBHOB *hob;     // back reference, so the window proc can tag its events
	REBCNT  flags;   // GUIW_* bits
	void   *widgets; // head of the child widget list (GUIWIDGET*)
	GUIFONT font;    // what a widget created from now on starts with; it is
	                 // read at creation and never again, so restyling a
	                 // window does not reach back into what it already holds

	// The menu bar. `menu` is the native object (HMENU / NSMenu*) and
	// `accel` the Win32 accelerator table, which has no counterpart on
	// macOS - a key equivalent there belongs to the item itself.
	//
	// The BLOCK the caller assigned is kept in hob->series, the one slot
	// the GC marks, which is free on a window - only an image widget uses
	// its own. That is what `win/menu` reads back.
	void   *menu;
	void   *accel;
	REBCNT *menu_ids;   // item id N (1-based) is the word menu_ids[N-1]
	REBYTE *menu_on;    // ... and menu_on[N-1] is whether it is enabled
	REBCNT  menu_count;
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
	                 // grouping note in gui-commands.c.
	                 // panel: GUI_PANEL_EDGE when it draws a frame - read by
	                 // the backend at paint time, so it can be turned on and
	                 // off without touching the native control
	REBCNT  color;   // text colour: 0 when the platform decides, otherwise
	                 // GUI_COLOR_SET | 0xRRGGBB. Kept here rather than in the
	                 // control because Win32 does not store one: the PARENT
	                 // is asked for it, message by message, as each control
	                 // is about to paint
} GUIWIDGET;

#define GUIW_VISIBLE  1

// Passed to Gui_Open_Window(). Everything a window's frame can be is
// decided at creation and changeable afterwards through `resizable?` and
// `border?`, so these say only what it STARTS as.
#define GUI_WIN_FIXED       1
#define GUI_WIN_BORDERLESS  2

// wid->state of a panel
#define GUI_PANEL_EDGE 1

// wid->color. The top byte is the "has one" flag, which is why a colour of
// 0.0.0 is still distinguishable from no colour at all.
#define GUI_COLOR_SET        0xFF000000
#define GUI_COLOR_HAS(c)     (((c) & GUI_COLOR_SET) != 0)
#define GUI_COLOR_R(c)       (((c) >> 16) & 0xFF)
#define GUI_COLOR_G(c)       (((c) >>  8) & 0xFF)
#define GUI_COLOR_B(c)        ((c)        & 0xFF)
#define GUI_COLOR_OF(r,g,b)  (GUI_COLOR_SET | ((REBCNT)(r) << 16) \
                                            | ((REBCNT)(g) <<  8) \
                                            |  (REBCNT)(b))
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
		menu            ;; a menu item was picked; value = its word, not a number
	]
	;; Words the menu dialect understands beyond the labels and the item
	;; ids themselves. The separator `---` is NOT here: it would generate
	;; `W_GUI_MENU____`, so it is mapped by name in Gui_Init() instead.
	menu: [
		shift control alt  ;; extra modifiers in a shortcut block
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
		scale    decimal!  none      "Device pixels per unit of size - 1.0 at 100%, 1.75 at 175%, 2.0 on a Retina Mac"
		resizable? logic!  logic!    "Whether the user can resize it"
		border?    logic!  logic!    "Whether it has a title bar and a frame; a borderless window cannot be moved or closed by the user"
		;; Defaults for widgets created AFTERWARDS - see the note in the README.
		font      string!  [string! none!] "Font family widgets are created with; none for the system font"
		font-size integer! [integer! none!] "Point size widgets are created with; none for the system size"
		bold?     logic!   logic!    "Whether widgets are created bold"
		italic?   logic!   logic!    "Whether widgets are created italic"
		menu      block!   [block! none!] "The menu bar, as the dialect described in the README; none removes it"
		menu-enabled? block! block!  "Which items are greyed out, as word/logic pairs; setting merges, it does not replace"
	]
	widget: [
		"GUI widget handle - a native control inside a window"
		;NAME    GET       SET       DESCRIPTION
		text     string!   string!   "Label or contents; the caption of a framed panel; the selected item of a drop-down, which is read-only; none for an image"
		items    block!    block!    "Strings a drop-down offers; none for other kinds"
		index    integer!  integer!  "Which item is picked, 1-based; 0 for none"
		image    image!    image!    "Image shown by an image widget, none for other kinds"
		size     pair!     pair!     "Size of the control"
		offset   pair!     pair!     "Position inside whatever holds it - a window or a panel"
		id       integer!  none      "Native control handle as an integer"
		kind     word!     none      "What the control is: button, image, text, field, area, check, radio, slider, progress or drop-down"
		value    percent!  [percent! decimal!] "Position of a slider or a progress bar; none for other kinds"
		state    logic!    logic!    "Whether a check or a radio is on; none for other kinds"
		edge     logic!    logic!    "Whether a panel draws a frame around itself; none for other kinds"
		;; Typography. Every kind which has `text` has these; the rest answer none.
		font      string!  [string! none!] "Font family; none puts it back to the system font"
		font-size integer! [integer! none!] "Point size; none puts it back to the system size"
		bold?     logic!   logic!    "Whether the text is bold"
		italic?   logic!   logic!    "Whether the text is italic"
		color     tuple!   [tuple! none!] "Text colour; none lets the platform decide"
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
		/fixed  "The user cannot resize it"
		/borderless "No title bar and no frame - see the note in the README"
	]
	close-window: ["Destroys the window" window [handle!]]
	show-window:  ["Makes the window visible" window [handle!]]
	hide-window:  ["Hides the window without destroying it" window [handle!]]
	poll-events: [
		"Dispatches pending OS messages and returns the collected events"
		/wait
		 timeout [number!] {Seconds to sleep for if there is nothing to report; woken early by anything the OS delivers}
	]
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
		/edge  "Draws a frame around it"
		/title text [string!] "Caption set into the frame; implies /edge"
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
		/rate delay [number!] {Longest it may sleep with nothing to do (default: 0.05)}
	][
		;; NOT `wait delay` between polls: this sleeps INSIDE the extension,
		;; on the OS queue, and wakes the moment anything arrives - which is
		;; what keeps a themed control's animation smooth and the window
		;; responsive without spinning. The delay is only a ceiling.
		delay: any [delay 0.05]
		forever [
			foreach [type source pos val] poll-events/wait delay [
				handler type source pos val
				;; `close` only reports the request - closing is ours to do
				if all [type = 'close  source = window] [
					close-window window
				]
				;; the handler is allowed to close it as well, and the rest
				;; of this batch would then refer to widgets which are gone
				unless window/open? [break]
			]
			unless window/open? [exit]
		]
	]
]
