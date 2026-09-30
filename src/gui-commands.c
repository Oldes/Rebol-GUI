//
// Project: Rebol/GUI extension
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
// Command implementations and the event queue.
//
// One function per command; the enum, the declarations, the dispatch table
// and the `_init` handler are all generated from gui.reb.
//
// Nothing here is platform specific - every OS call goes through the small
// Gui_* backend declared in gui.h.
//

#include "gen-gui.h"
#include "gui.h"
#include <stdio.h>
#include <string.h>
// MAKE_MEM / FREE_MEM are malloc and free behind a macro, and nothing in
// rebol-extension.h declares either - a window's default font name is the
// one thing this file allocates for itself.
#include <stdlib.h>

static const REBYTE* ERR_INVALID_HANDLE = (const REBYTE*)"Invalid GUI window handle!";
static const REBYTE* ERR_NO_HANDLE      = (const REBYTE*)"Failed to create the window handle!";
static const REBYTE* ERR_NO_WINDOW      = (const REBYTE*)"Failed to create the window!";
static const REBYTE* ERR_BAD_SIZE       = (const REBYTE*)"Size must be positive!";
static const REBYTE* ERR_NO_WIDGET      = (const REBYTE*)"Failed to create the widget!";
static const REBYTE* ERR_BAD_IMAGE      = (const REBYTE*)"Empty or invalid image!";
static const REBYTE* ERR_NO_PORT_STATE  = (const REBYTE*)"Not a usable port!";
static const REBYTE* ERR_DEVICE_FAIL    = (const REBYTE*)"GUI device command failed!";
static const REBYTE* ERR_HAS_MODAL      = (const REBYTE*)"The window has a modal dialog open!";
static const REBYTE* ERR_MODAL_DEPTH    = (const REBYTE*)"Too many modal dialogs open!";
static const REBYTE* ERR_BAD_COLUMNS    = (const REBYTE*)"Columns must be titles, each optionally followed by a width!";
static const REBYTE* ERR_BAD_CELLS      = (const REBYTE*)"The cells do not make whole rows!";

// The device request behind a port. A port's state field is where the host
// keeps it, and this is the only way an extension can reach the device it
// registered: there is no OS_Do_Device in the RL_ API.
#define GUI_DEV_REQ(n) RL_PORT_STATE(RXA_PORT(frm, n), (REBCNT)Gui_Dev_Id)

// The menu dialect's separator. Not in a `words:` list because to-c-name
// would spell it W_GUI_MENU____; it is one symbol, so it is mapped by name
// in Gui_Init() instead. See Block_To_Menu().
REBCNT Word_Separator = 0;


//== event queue ==============================================================
//
// A fixed ring buffer, written by the window procedure and drained by
// `poll-events`. Deliberately allocation free: the producer may run inside a
// modal OS loop (moving or resizing a window), where calling back into the
// interpreter to grow a series would be a very bad idea.
//
// The size must stay a power of two - the free running counters are masked,
// not wrapped, so that `Head - Tail` is the count even across overflow.

#define GUI_QUEUE_SIZE 512
#define GUI_QUEUE_MASK (GUI_QUEUE_SIZE - 1)

static GUIEVT Event_Queue[GUI_QUEUE_SIZE];
static REBCNT Event_Head = 0;    // next slot to be written
static REBCNT Event_Tail = 0;    // next slot to be read
static REBCNT Event_Dropped = 0; // reported once, by the next poll

#define QUEUE_COUNT() ((REBCNT)(Event_Head - Event_Tail))
#define QUEUE_AT(n)   (&Event_Queue[((Event_Tail) + (n)) & GUI_QUEUE_MASK])


/***********************************************************************
**  Appends an event, unless the queue is full.
**
**  The source is a handle context - or, for an event about a screen,
**  NULL with the screen's key: making a screen's handle allocates, which
**  the pump must not do, so `poll-events` makes it later.
**
**  Consecutive `move` events of the same source are collapsed onto the
**  last one: a fast mouse produces hundreds of them per second and only
**  the newest position is of any use.
***********************************************************************/
static REBOOL Same_Source(const GUIEVT *evt, REBHOB *source, const REBYTE *screen)
{
	if (source) return evt->source == source;
	return !evt->source && screen
	    && strncmp((const char*)evt->screen, (const char*)screen, GUI_SCREEN_KEY) == 0;
}

/***********************************************************************
**  The modal stack.
**
**  Every window opened with `/modal`, oldest first. While it is not
**  empty, the newest is the only window taking input: every other one
**  is blocked, and its events are dropped here, at the queue - so this
**  holds for `do-events` and for a loop of a script's own alike.
**
**  Three kinds still get through from a blocked window: `theme-change`
**  and `resize`, which a script needs to keep that window right, and
**  `leave`, so that what the pointer was over is still let go of.
***********************************************************************/
#define GUI_MODAL_MAX 16
static GUIWIN *Modal_Stack[GUI_MODAL_MAX];
static REBCNT  Modal_Depth = 0;

GUIWIN* Gui_Modal_Top(void)
{
	return Modal_Depth ? Modal_Stack[Modal_Depth - 1] : NULL;
}

REBOOL Gui_Window_Blocked(GUIWIN *win)
{
	return (win && Modal_Depth && win != Gui_Modal_Top()) ? TRUE : FALSE;
}

// Takes a window off the stack, wherever it is, and tells the backend -
// before the window goes, so that input can be given back first.
static void Modal_Remove(GUIWIN *win)
{
	REBCNT n, kept = 0;
	REBOOL found = FALSE;

	for (n = 0; n < Modal_Depth; n++) {
		if (Modal_Stack[n] == win) { found = TRUE; continue; }
		Modal_Stack[kept++] = Modal_Stack[n];
	}
	if (!found) return;
	Modal_Depth = kept;
	Gui_Apply_Modal(Gui_Modal_Top());
}

// Whether a modal dialog is open on `win` - directly, or through a dialog
// opened on one of those.
static REBOOL Has_Modal(GUIWIN *win)
{
	REBCNT n;
	for (n = 0; n < Modal_Depth; n++) {
		GUIWIN *owner = (GUIWIN*)Modal_Stack[n]->modal_owner;
		for (; owner; owner = (GUIWIN*)owner->modal_owner)
			if (owner == win) return TRUE;
	}
	return FALSE;
}

// The window an event source belongs to, or NULL for a screen.
static GUIWIN* Window_Of_Source(REBHOB *source)
{
	if (!source || !source->data) return NULL;
	if (source->sym == Handle_GuiWindow) return (GUIWIN*)source->data;
	if (source->sym == Handle_GuiWidget) return ((GUIWIDGET*)source->data)->owner;
	return NULL;
}

static REBOOL Modal_Drops(REBHOB *source, REBCNT type)
{
	if (!Modal_Depth) return FALSE;
	if (type == EVT_THEME_CHANGE || type == EVT_RESIZE || type == EVT_LEAVE) return FALSE;
	return Gui_Window_Blocked(Window_Of_Source(source));
}

static void Append_Event(REBHOB *source, const REBYTE *screen, REBCNT type,
                         REBINT x, REBINT y, REBINT value)
{
	GUIEVT *evt;

	if (Modal_Drops(source, type)) return;

	if (type == EVT_MOVE && QUEUE_COUNT() > 0) {
		evt = QUEUE_AT(QUEUE_COUNT() - 1);
		if (evt->type == EVT_MOVE && Same_Source(evt, source, screen)) {
			evt->x = x;
			evt->y = y;
			evt->value = value;
			return;
		}
	}

	if (QUEUE_COUNT() >= GUI_QUEUE_SIZE) {
		Event_Dropped++;
		return;
	}

	evt = &Event_Queue[Event_Head & GUI_QUEUE_MASK];
	CLEARS(evt);           // `drop` is NULL for everything but a drop event
	evt->source = source;
	evt->type   = type;
	evt->x      = x;
	evt->y      = y;
	evt->value  = value;
	if (!source && screen) strncpy((char*)evt->screen, (const char*)screen, GUI_SCREEN_KEY - 1);
	Event_Head++;
}


/***********************************************************************
**  `enter` and `leave`: what the pointer is over.
**
**  Worked out from the moves themselves, here, rather than asked of
**  either platform: a `move` already names the deepest widget under the
**  pointer (or the window, or with `track-mouse` the screen), so the
**  thing the pointer is over changes exactly when a move arrives from a
**  different source. That is when the old one gets `leave` and the new
**  one `enter` - before the move itself, so a handler sees
**  leave, enter, move in that order.
**
**  Each is positional like `move`: `enter` where the pointer came in,
**  `leave` where it was last seen over the thing it left - each in that
**  source's own coordinates (its window's client area, or its screen).
**
**  What the moves cannot show is the pointer leaving every window with
**  nothing to report it next. The backends notice that (WM_MOUSELEAVE,
**  mouseExited:) and call Gui_Pointer_Left().
**
**  During a press every move comes from the control that was pressed,
**  wherever the pointer is - so nothing changes hands until the button
**  is up, the same as with pointer capture in a browser.
***********************************************************************/
static struct {
	REBHOB *hob;                     // what the pointer is over, or NULL
	REBYTE  screen[GUI_SCREEN_KEY];  // ... or a screen, by key
	REBINT  x, y;                    // where it was last seen over it
} Hover;

static REBOOL Hovering(void) { return Hover.hob || Hover.screen[0]; }

static void Hover_To(REBHOB *source, const REBYTE *screen, REBINT x, REBINT y)
{
	REBOOL same = source
		? (Hover.hob == source)
		: (!Hover.hob && screen && Hover.screen[0]
		   && strncmp((const char*)Hover.screen, (const char*)screen, GUI_SCREEN_KEY) == 0);

	if (!same) {
		if (Hovering())
			Append_Event(Hover.hob, Hover.screen, EVT_LEAVE, Hover.x, Hover.y, 0);
		Hover.hob = source;
		Hover.screen[0] = 0;
		if (!source && screen) strncpy((char*)Hover.screen, (const char*)screen, GUI_SCREEN_KEY - 1);
		Append_Event(source, screen, EVT_ENTER, x, y, 0);
	}
	Hover.x = x;
	Hover.y = y;
}

void Gui_Pointer_Left(void)
{
	if (!Hovering()) return;
	Append_Event(Hover.hob, Hover.screen, EVT_LEAVE, Hover.x, Hover.y, 0);
	Hover.hob = NULL;
	Hover.screen[0] = 0;
}

// A handle going away takes its hover with it - it can report nothing,
// and the next move reports `enter` on whatever is there instead.
static void Forget_Hover(REBHOB *hob)
{
	if (hob && Hover.hob == hob) Hover.hob = NULL;
}

void Gui_Queue_Event(REBHOB *source, REBCNT type, REBINT x, REBINT y, REBINT value)
{
	if (!source) return;
	if (type == EVT_MOVE) Hover_To(source, NULL, x, y);
	Append_Event(source, NULL, type, x, y, value);
}

// The code rides in `x`: a key event has no position, and the drain
// knows these types by their EVT_* code.
void Gui_Queue_Key(REBHOB *source, REBCNT type, REBU32 code, REBINT mods)
{
	if (!source || !code) return;
	Append_Event(source, NULL, type, (REBINT)code, 0, mods);
}


/***********************************************************************
**  `track-mouse`: moves over the screens, outside this program.
**
**  Nothing in the OS reports those to a program which does not own the
**  window under the pointer, so the pointer is ASKED for, after every
**  pump - which is already running on every WAIT. A position that has
**  not changed queues nothing, so an idle pointer costs one system call
**  per poll and no events.
**
**  Moves over this program's own windows are left to the windows, which
**  report them with the widget under the pointer as the source; the
**  tracker only covers everywhere else. Forgetting the last position
**  while the pointer is over a window is what makes leaving one report
**  at once, even at the point it went in.
**
**  The source is the screen, and the position is within that screen,
**  from its top-left corner - so `evt/offset + evt/source/offset` is on
**  the desktop, in the space window offsets use.
***********************************************************************/
static REBOOL Track_Pointer = FALSE;
static REBINT Tracked_X = 0, Tracked_Y = 0;
static REBYTE Tracked_Key[GUI_SCREEN_KEY];

REBOOL Gui_Tracking_Pointer(void) { return Track_Pointer; }

static void Queue_Screen_Move(const REBYTE *key, REBINT x, REBINT y, REBINT mods)
{
	Hover_To(NULL, key, x, y);
	Append_Event(NULL, key, EVT_MOVE, x, y, mods);
}

void Gui_Track_Pointer(void)
{
	REBYTE key[GUI_SCREEN_KEY];
	REBINT x = 0, y = 0, mods = 0;
	REBOOL ours = FALSE;

	if (!Track_Pointer) return;
	CLEARS(&key);
	if (!Gui_Pointer_At(key, &x, &y, &mods, &ours) || !key[0]) return;

	if (ours) {
		Tracked_Key[0] = 0;
		return;
	}
	if (x == Tracked_X && y == Tracked_Y
	    && strncmp((const char*)key, (const char*)Tracked_Key, GUI_SCREEN_KEY) == 0)
		return;

	Tracked_X = x;
	Tracked_Y = y;
	memcpy(Tracked_Key, key, GUI_SCREEN_KEY);
	Queue_Screen_Move(key, x, y, mods);
}


/***********************************************************************
**  A drop payload: plain C memory, grown one string at a time.
**
**  Neither OS hands its strings over in one block - Win32 asks for them
**  by index and AppKit has an array of NSStrings - so this appends, and
**  keeps them NUL separated in one buffer rather than as an array of
**  pointers. One allocation to free, and `poll-events` walks it once.
***********************************************************************/
GUIDROPDATA *Gui_Drop_Payload(REBCNT kind, REBCNT size)
{
	GUIDROPDATA *data = (GUIDROPDATA*)MAKE_CLEAR_MEM(sizeof(GUIDROPDATA));
	if (!data) return NULL;

	if (size < 256) size = 256;
	data->text = (REBYTE*)MAKE_MEM(size);
	if (!data->text) {
		FREE_MEM(data);
		return NULL;
	}
	data->kind     = kind;
	data->capacity = size;
	data->text[0]  = 0;    // size and count are already zero
	return data;
}


REBOOL Gui_Drop_Append(GUIDROPDATA *data, const REBYTE *utf8, REBCNT len)
{
	if (!data || !utf8) return FALSE;

	if (data->size + len + 1 > data->capacity) {
		REBCNT want = (data->size + len + 1) * 2;
		REBYTE *grown = (REBYTE*)MAKE_MEM(want);
		if (!grown) return FALSE;
		COPY_MEM(grown, data->text, data->size);
		FREE_MEM(data->text);
		data->text = grown;
		data->capacity = want;
	}

	COPY_MEM(data->text + data->size, utf8, len);
	data->size += len;
	data->text[data->size++] = 0;
	data->count++;
	return TRUE;
}


void Gui_Drop_Free(GUIDROPDATA *data)
{
	if (!data) return;
	if (data->text) FREE_MEM(data->text);
	FREE_MEM(data);
}


/***********************************************************************
**  Queues a drop.
**
**  The queue takes the payload whether the event fits or not, which is
**  the only way a full queue cannot leak it - the backend has already
**  told the OS the drop was accepted by the time it gets here.
***********************************************************************/
void Gui_Queue_Drop(REBHOB *target, GUIDROPDATA *data, REBINT x, REBINT y)
{
	GUIEVT *evt;

	if (!data) return;
	if (!target || QUEUE_COUNT() >= GUI_QUEUE_SIZE
	    || Modal_Drops(target, EVT_DROP_FILE)) {
		Event_Dropped++;
		Gui_Drop_Free(data);
		return;
	}

	evt = &Event_Queue[Event_Head & GUI_QUEUE_MASK];
	CLEARS(evt);
	evt->source = target;
	evt->type   = (data->kind == GUI_DROP_TEXT) ? EVT_DROP_TEXT : EVT_DROP_FILE;
	evt->x      = x;
	evt->y      = y;
	evt->drop   = data;
	Event_Head++;
}


/***********************************************************************
**  Removes every queued event of one source.
**
**  Queued events hold the source's handle context, and closing a window or
**  a widget drops the lock which kept that context alive - so anything
**  still referring to it has to go at the same moment.
***********************************************************************/
static void Purge_Events(REBHOB *source)
{
	REBCNT count = QUEUE_COUNT();
	REBCNT kept = 0;
	REBCNT n;

	for (n = 0; n < count; n++) {
		GUIEVT *evt = QUEUE_AT(n);
		if (evt->source == source) {
			// The payload is the queue's to free - nothing else holds it
			// until `poll-events` turns it into Rebol values.
			Gui_Drop_Free(evt->drop);
			continue;
		}
		if (kept != n) *QUEUE_AT(kept) = *evt;
		kept++;
	}
	Event_Head = Event_Tail + kept;
}


/***********************************************************************
**  What the device poll asks before doing anything.
**
**  The window count is kept rather than derived: there is no global
**  window list, and the poll runs on every WAIT the program makes, so
**  the answer has to be a load rather than a walk.
***********************************************************************/
static REBCNT Open_Windows = 0;

REBOOL Gui_Windows_Open(void)
{
	return Open_Windows > 0 ? TRUE : FALSE;
}

// What RDC_READ reports for `read gui/event-port`.
REBCNT Gui_Event_Count(void)
{
	return QUEUE_COUNT();
}


/***********************************************************************
**  Answers TRUE exactly once per batch: the device rings its doorbell on
**  the first poll which finds the queue non-empty, and not again until
**  `poll-events` has drained it.
**
**  Why "anything waiting" rather than "anything new since the pump": not
**  every event arrives during a pump. A programmatic resize dispatches
**  WM_SIZE inside SetWindowPos, so the event is queued from the middle of
**  `win/size:` - and the user dragging a window runs a modal OS loop
**  which dispatches for itself and never comes through Gui_Pump at all.
**
**  And why it is asked HERE rather than done in Gui_Queue_Event, which
**  would be the obvious place: that function is the one thing in this
**  file which may run inside such a modal loop, where calling RL_Event
**  would grow a Rebol series - see the note above it. The poll is on the
**  interpreter's own thread of control and is safe.
***********************************************************************/
static REBOOL Event_Rung = FALSE;

REBOOL Gui_Ring_Doorbell(void)
{
	if (QUEUE_COUNT() == 0 || Event_Rung) return FALSE;
	Event_Rung = TRUE;
	return TRUE;
}


// Undoes what keeps a handle context alive while its native object exists.
static void Release_Handle(REBHOB *hob)
{
	if (!hob) return;
	Forget_Hover(hob);
	Purge_Events(hob);
	hob->flags &= ~HANDLE_CONTEXT_LOCKED;
}


/***********************************************************************
**  Radio groups.
**
**  Neither platform groups radios the way a caller means it. Win32 goes
**  by sibling order bounded by WS_GROUP flags; AppKit goes by superview
**  and action selector - and since every control here is a direct child
**  of the window with the same action, both would put every radio of a
**  window into one group.
**
**  So the grouping is done here instead, over the window's own widget
**  list, and `wid->state` - not the native control - is the truth.
**
**  Every radio in the window is then written, not just this group's: a
**  click on one radio makes AppKit clear the others behind our back, so
**  the ones it touched have to be put back. That only works because
**  Gui_Widget_Set_State() writes a state without AppKit reading it as a
**  group operation - see the note on it in gui-mac.m. Writing them in
**  two passes, with the one being switched on written LAST, keeps the
**  right control selected even if some platform still insists on
**  clearing siblings when a radio goes on.
**
**  NOTE: the widget list is in reverse creation order, because widgets
**  are prepended to it. Nothing here may depend on that order.
***********************************************************************/
static void Sync_Radio_Group(GUIWIDGET *wid, REBOOL on)
{
	GUIWIDGET *other;

	if (!wid || !wid->owner) return;

	wid->state = on ? 1 : 0;

	if (on) {
		for (other = (GUIWIDGET*)wid->owner->widgets; other;
		     other = (GUIWIDGET*)other->next)
		{
			if (other != wid
			 && other->kind  == W_GUI_WIDGET_RADIO
			 && other->group == wid->group) other->state = 0;
		}
	}

	// pass 1: everything which should be off
	for (other = (GUIWIDGET*)wid->owner->widgets; other;
	     other = (GUIWIDGET*)other->next)
	{
		if (other->kind == W_GUI_WIDGET_RADIO && other->handle && !other->state)
			Gui_Widget_Set_State(other, FALSE);
	}

	// pass 2: everything which should be on, except this one
	for (other = (GUIWIDGET*)wid->owner->widgets; other;
	     other = (GUIWIDGET*)other->next)
	{
		if (other != wid
		 && other->kind == W_GUI_WIDGET_RADIO && other->handle && other->state)
			Gui_Widget_Set_State(other, TRUE);
	}

	// pass 3: and this one last of all
	if (wid->handle && wid->state) Gui_Widget_Set_State(wid, TRUE);
}


/***********************************************************************
**  A control was activated - a button pressed, a box ticked.
**
**  A checkbox has already toggled itself by the time this runs, so its
**  state is only read back; a radio is switched on and its group
**  settled. Everything else just reports the click.
***********************************************************************/
void Gui_Widget_Activated(GUIWIDGET *widget, REBINT x, REBINT y, REBINT flags)
{
	if (!widget || !widget->hob) return;

	switch (widget->kind) {
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_TOGGLE:
		widget->state = Gui_Widget_Get_State(widget) ? 1 : 0;
		break;
	case W_GUI_WIDGET_RADIO:
		// Clicking a radio always turns it on - there is no untick.
		Sync_Radio_Group(widget, TRUE);
		break;
	}

	Gui_Queue_Event(widget->hob, EVT_CLICK, x, y, flags);
}


// Fills an RXIARG with a handle context; defined further down, where the
// rest of the argument helpers are.
static void Set_Handle_Arg(RXIARG *arg, REBHOB *hob);
static REBHOB *Screen_Handle(const REBYTE *key);

/***********************************************************************
**  The one GC-marked slot, shared.
**
**  A handle context has exactly ONE series the collector marks, and
**  three kinds need two things kept alive in it: whatever the kind
**  itself holds, and the children once it has any. A window holds the
**  menu block it was given; an image widget holds its image!.
**
**  So the slot is a BLOCK of exactly two values, with fixed meanings:
**
**      [0] the payload  - an image! for an image widget, the menu block
**                         for a window, none for anything else
**      [1] the children - a block of handles, or none
**      [2] a list-view's scratch string, which each cell is formed into
**          (only a list-view has this third slot - see Hob_Scratch)
**      [3] a list-view's columns spec, normalized - see List_Spec
**
**  Marking the outer block marks both, and nothing outside these four
**  functions knows the layout. A widget which is neither a container
**  nor an image never allocates one.
***********************************************************************/
#define SLOT_PAYLOAD  0
#define SLOT_CHILDREN 1
#define SLOT_SCRATCH  2

static REBSER *Hob_Slots(REBHOB *hob)
{
	REBSER *blk;
	RXIARG  none;

	if (!hob) return NULL;
	if (hob->series) return hob->series;

	blk = (REBSER*)RL_MAKE_BLOCK(2);
	if (!blk) return NULL;

	// Stored BEFORE anything else can allocate: until it is in the slot
	// the GC marks, nothing references this block and a collection in
	// the middle of filling it would take it away.
	hob->series = blk;

	// Both slots exist from the start, so the layout never depends on
	// which of the two was assigned first.
	CLEARS(&none);
	RL_SET_VALUE(blk, SLOT_PAYLOAD,  none, RXT_NONE);
	RL_SET_VALUE(blk, SLOT_CHILDREN, none, RXT_NONE);

	return blk;
}


// The payload as a plain series - an image's pixels, a menu's block - or
// NULL when there is none.
static REBSER *Hob_Payload(REBHOB *hob)
{
	RXIARG val;
	REBCNT type;

	if (!hob || !hob->series) return NULL;
	type = RL_GET_VALUE(hob->series, SLOT_PAYLOAD, &val);
	if (type != RXT_IMAGE && type != RXT_BLOCK) return NULL;

	// An image! and a block! carry their series in the same place. The
	// index is deliberately not read: for an image it overlaps the
	// dimensions, which is the trap this extension has been caught by
	// before.
	return (REBSER*)val.series;
}


// Replaces the payload. A type of RXT_NONE clears it.
static REBOOL Hob_Set_Payload(REBHOB *hob, RXIARG *val, REBCNT type)
{
	REBSER *blk = Hob_Slots(hob);
	RXIARG  none;

	if (!blk) return FALSE;
	if (!val) {
		CLEARS(&none);
		val  = &none;
		type = RXT_NONE;
	}
	RL_SET_VALUE(blk, SLOT_PAYLOAD, *val, (int)type);
	return TRUE;
}


// The children block. `make` decides whether an absent one is created,
// so a read of `children` on a childless container allocates nothing.
static REBSER *Hob_Children(REBHOB *hob, REBOOL make)
{
	RXIARG  val;
	REBSER *blk, *kids;

	if (!hob) return NULL;

	if (hob->series
	    && RL_GET_VALUE(hob->series, SLOT_CHILDREN, &val) == RXT_BLOCK)
		return (REBSER*)val.series;

	if (!make) return NULL;

	blk = Hob_Slots(hob);
	if (!blk) return NULL;

	kids = (REBSER*)RL_MAKE_BLOCK(4);
	if (!kids) return NULL;

	CLEARS(&val);
	val.series = kids;
	val.index  = 0;
	// Protected across the store: the block is referenced by nothing
	// until it is in the slot.
	RL_PROTECT_GC(kids, 1);
	RL_SET_VALUE(blk, SLOT_CHILDREN, val, RXT_BLOCK);
	RL_PROTECT_GC(kids, 0);

	return kids;
}


/***********************************************************************
**  list-view: the cells, and the scratch string they are formed into.
**
**  `items` is NOT copied. The block the script assigned is kept in the
**  payload slot as it was given, index included, and the cells are read
**  out of it whenever the control asks - so `lv/items` reads back that
**  very block, with its values unchanged (a copy made through RXIARG
**  would lose word bindings, for one). The price is the one a shared
**  block always has: change it in place and the control shows the new
**  values only as it repaints them, and a new NUMBER of rows only once
**  the block is set again - `lv/items: lv/items`.
***********************************************************************/

// Longest a cell is formed, in chars. Past it the cell ends with "...":
// a cell holding a big block or a long string must not cost its whole
// FORM each time it is painted.
#define LIST_CELL_LIMIT 256

// The items block and its index, or NULL.
static REBSER *List_Items(GUIWIDGET *wid, REBCNT *index)
{
	RXIARG val;
	if (!wid || !wid->hob || !wid->hob->series) return NULL;
	if (RL_GET_VALUE(wid->hob->series, SLOT_PAYLOAD, &val) != RXT_BLOCK) return NULL;
	if (index) *index = val.index;
	return (REBSER*)val.series;
}

// Made when the list-view is created, which is where allocating is
// comfortable - not at paint time.
static REBSER *Hob_Scratch(REBHOB *hob, REBOOL make)
{
	REBSER *blk, *str;
	RXIARG  val;

	if (!hob) return NULL;
	if (hob->series && RL_GET_VALUE(hob->series, SLOT_SCRATCH, &val) == RXT_STRING)
		return (REBSER*)val.series;
	if (!make) return NULL;

	blk = Hob_Slots(hob);
	if (!blk) return NULL;
	str = (REBSER*)RL_MAKE_STRING(LIST_CELL_LIMIT + 8, FALSE);
	if (!str) return NULL;

	CLEARS(&val);
	val.series = str;
	val.index  = 0;
	RL_PROTECT_GC(str, 1);
	RL_SET_VALUE(blk, SLOT_SCRATCH, val, RXT_STRING);  // appends: slot [2]
	RL_PROTECT_GC(str, 0);
	return str;
}

REBCNT Gui_List_Rows(GUIWIDGET *wid)
{
	REBCNT  index = 0, tail, cols;
	REBSER *blk = List_Items(wid, &index);

	if (!blk) return 0;
	cols = wid->fields;
	if (cols == 0) return 0;
	tail = (REBCNT)RL_SERIES(blk, RXI_SER_TAIL);
	return (tail > index) ? (tail - index) / cols : 0;
}

REBOOL Gui_List_Cell(GUIWIDGET *wid, REBCNT row, REBCNT col, REBYTE **utf8, REBCNT *len)
{
	REBCNT  index = 0, cols, type;
	REBSER *blk = List_Items(wid, &index);
	REBSER *str;
	RXIARG  val;
	REBINT  n;

	*utf8 = (REBYTE*)"";
	*len  = 0;
	if (!blk) return FALSE;
	cols = wid->fields;
	if (col >= cols) return FALSE;

	type = RL_GET_VALUE(blk, index + row * cols + col, &val);
	if (type == 0 || type == RXT_END) return FALSE;  // the block got shorter
	// A missing value is an empty cell, not the word "none".
	if (type == RXT_NONE || type == RXT_UNSET) return TRUE;

	str = Hob_Scratch(wid->hob, FALSE);
	if (!str) return FALSE;
	n = RL_FORM_VALUE(str, val, type, LIST_CELL_LIMIT, 0);
	if (n < 0) return FALSE;

	// Straight from the series, as IMG_DATA reads an image's pixels - NOT
	// through RL_SERIES(RXI_SER_DATA). That returns the pointer as a
	// REBUPT, and where the extension's build types REBUPT as a 32-bit
	// `unsigned long` (a 64-bit Windows compiler which is not C99 and
	// defines no __LLP64__), the address comes back cut in half.
	*utf8 = STR_HEAD(str);
	*len  = (REBCNT)n;
	return TRUE;
}

void Gui_List_Picked(GUIWIDGET *wid, REBINT n)
{
	REBINT x = 0, y = 0, w = 0, h = 0;

	if (!wid || n == wid->picked) return;
	wid->picked = n;
	if (!wid->hob) return;
	Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
	Gui_Queue_Event(wid->hob, EVT_CHANGE, x, y, 0);
}

// Picks row `n` (0-based; out of range picks nothing) without it being
// reported - the row is recorded as reported first.
static void List_Set_Index(GUIWIDGET *wid, REBINT n)
{
	if (n < 0 || n >= (REBINT)Gui_List_Rows(wid)) n = -1;
	wid->picked = n;
	Gui_Widget_Set_Index(wid, n);
}

/***********************************************************************
**  The column spec: titles, each optionally followed by a width and an
**  alignment, in either order.
**
**      ["Name" 120  "Size" 60 right  "Date" none center]
**
**  A width of none fits the title - except on the LAST column, which
**  then fills whatever room the others leave, and keeps filling it as
**  the list is resized. The alignment is one of left (the default),
**  center and right, and applies to the title and the cells alike.
**
**  Checked whole before anything changes, so a bad spec leaves the
**  columns as they were.
***********************************************************************/
// A width of none, written in a block that is not reduced, is the WORD
// `none` - which is what the spec in the README looks like.
static REBOOL Is_None_Width(REBCNT type, RXIARG *val)
{
	static u32 none_sym = 0;
	if (type == RXT_NONE) return TRUE;
	if (type != RXT_WORD) return FALSE;
	if (!none_sym) none_sym = RL_MAP_WORD((REBYTE*)"none");
	return ((u32)val->int32a == none_sym) ? TRUE : FALSE;
}

// GUI_ALIGN_* for an alignment word, or -1 for anything else.
static REBINT Align_Of(REBCNT type, RXIARG *val)
{
	if (type != RXT_WORD) return -1;
	switch (RL_FIND_WORD(Gui_align_words, (REBCNT)val->int32a)) {
	case W_GUI_ALIGN_LEFT:   return GUI_ALIGN_LEFT;
	case W_GUI_ALIGN_CENTER: return GUI_ALIGN_CENTER;
	case W_GUI_ALIGN_RIGHT:  return GUI_ALIGN_RIGHT;
	}
	return -1;
}

typedef struct {
	REBCNT  index;   // of the title in the block
	REBINT  width;   // -1 for none
	REBINT  align;   // GUI_ALIGN_*
} LIST_COL;

// Reads the column at `index` - the title and what follows it - and
// returns the index after it, or 0 when the spec is bad there.
static REBCNT List_Next_Column(REBSER *blk, REBCNT index, LIST_COL *col)
{
	REBCNT type;
	RXIARG val;
	REBOOL has_width = FALSE, has_align = FALSE;

	type = RL_GET_VALUE(blk, index, &val);
	if (type != RXT_STRING) return 0;
	col->index = index++;
	col->width = -1;
	col->align = GUI_ALIGN_LEFT;

	for (;; index++) {
		REBINT a;
		type = RL_GET_VALUE(blk, index, &val);
		if (type == 0 || type == RXT_END || type == RXT_STRING) break;
		if (!has_width && (type == RXT_INTEGER || Is_None_Width(type, &val))) {
			has_width = TRUE;
			if (type == RXT_INTEGER) col->width = (val.int64 < 0) ? -1 : (REBINT)val.int64;
			continue;
		}
		if (!has_align && (a = Align_Of(type, &val)) >= 0) {
			has_align = TRUE;
			col->align = a;
			continue;
		}
		return 0;   // a second width, a second alignment, or anything else
	}
	return index;
}

static REBOOL List_Columns_Valid(REBSER *blk, REBCNT index, REBCNT *count)
{
	REBCNT   type, n = 0;
	RXIARG   val;
	LIST_COL col;

	while ((type = RL_GET_VALUE(blk, index, &val)) != 0 && type != RXT_END) {
		index = List_Next_Column(blk, index, &col);
		if (!index) return FALSE;
		n++;
	}
	*count = n;
	return TRUE;
}

// Where the columns spec is kept, normalized: three values per column -
// the title, the width (an integer, 0 for hidden, or none) and the
// alignment word. `columns` is read back from it. Hidden columns exist
// ONLY here: the native control is given the visible ones, each told which
// value of a row it shows. Kept in the handle's slots, after the scratch
// string (see the note on the shared slot).
#define SLOT_COLUMNS 3

static REBSER *List_Spec(GUIWIDGET *wid)
{
	RXIARG val;
	if (!wid || !wid->hob || !wid->hob->series) return NULL;
	if (RL_GET_VALUE(wid->hob->series, SLOT_COLUMNS, &val) != RXT_BLOCK) return NULL;
	return (REBSER*)val.series;
}

static REBOOL List_Set_Columns(GUIWIDGET *wid, REBSER *blk, REBCNT index)
{
	REBCNT   type, count = 0, field = 0;
	RXIARG   val;
	LIST_COL col;
	REBOOL   fill = FALSE, titled = FALSE, any_shown = FALSE;
	REBSER  *spec, *slots;
	REBCNT   old = wid->fields;

	if (!List_Columns_Valid(blk, index, &count)) return FALSE;

	// The normalized spec first - it is what `columns` reads back.
	slots = Hob_Slots(wid->hob);
	spec  = (REBSER*)RL_MAKE_BLOCK(count * 3);
	if (!slots || !spec) return FALSE;
	RL_PROTECT_GC(spec, 1);

	Gui_List_Clear_Columns(wid);

	while ((type = RL_GET_VALUE(blk, index, &val)) != 0 && type != RXT_END) {
		REBYTE *utf8 = NULL;
		RXIARG  title, w, a;
		int     len;

		index = List_Next_Column(blk, index, &col);   // valid: checked above
		RL_GET_VALUE(blk, col.index, &title);
		RL_SET_VALUE(spec, field * 3, title, RXT_STRING);
		CLEARS(&w);
		if (col.width < 0) {
			RL_SET_VALUE(spec, field * 3 + 1, w, RXT_NONE);
		} else {
			w.int64 = (i64)col.width;
			RL_SET_VALUE(spec, field * 3 + 1, w, RXT_INTEGER);
		}
		CLEARS(&a);
		a.int32a = (i32)Gui_align_words[col.align == GUI_ALIGN_RIGHT  ? W_GUI_ALIGN_RIGHT
		                              : col.align == GUI_ALIGN_CENTER ? W_GUI_ALIGN_CENTER
		                              : W_GUI_ALIGN_LEFT];
		RL_SET_VALUE(spec, field * 3 + 2, a, RXT_WORD);

		// A width of 0 is a hidden column: in the rows, not on screen.
		if (col.width != 0) {
			len = RL_GET_UTF8_STRING((REBSER*)title.series, title.index, (void**)&utf8);
			Gui_List_Add_Column(wid, field, utf8, (REBCNT)(len < 0 ? 0 : len),
			                    col.width, col.align);
			if (len > 0) titled = TRUE;
			fill      = (col.width < 0);   // what the last SHOWN one says counts
			any_shown = TRUE;
		}
		field++;
	}

	CLEARS(&val);
	val.series = spec;
	val.index  = 0;
	RL_SET_VALUE(slots, SLOT_COLUMNS, val, RXT_BLOCK);   // [3], after the scratch
	RL_PROTECT_GC(spec, 0);

	wid->fields = count;
	if (fill && any_shown) wid->state |= GUI_LIST_FILL;
	else                   wid->state &= ~GUI_LIST_FILL;
	Gui_List_Fill(wid);

	// No titles at all: no header row. (One title is enough to keep it -
	// the others are simply blank.)
	Gui_List_Show_Header(wid, titled);

	// Rows are cells over columns: a different number of columns makes
	// the cells mean something else, so they go.
	if (count != old) Hob_Set_Payload(wid->hob, NULL, RXT_NONE);
	wid->sort = 0;
	Gui_List_Show_Sort(wid);
	List_Set_Index(wid, -1);
	Gui_List_Reload(wid, Gui_List_Rows(wid));
	return TRUE;
}

// The spec back: title, width - 0 for hidden, none for a last column which
// fills, and otherwise the width it has NOW, which the user may have
// dragged - and the alignment when it is not left. Setting it again gives
// the same columns.
static REBSER *List_Columns_Block(GUIWIDGET *wid)
{
	REBSER *spec = List_Spec(wid);
	REBCNT  n, at = 0, count = wid->fields, last_shown = (REBCNT)-1;
	REBSER *blk;
	RXIARG  title, w, a;

	if (!spec) return (REBSER*)RL_MAKE_BLOCK(0);
	for (n = 0; n < count; n++) {
		if (RL_GET_VALUE(spec, n * 3 + 1, &w) != RXT_INTEGER || w.int64 != 0) last_shown = n;
	}

	blk = (REBSER*)RL_MAKE_BLOCK(count * 3);
	if (!blk) return NULL;
	RL_PROTECT_GC(blk, 1);
	for (n = 0; n < count; n++) {
		REBCNT wtype;
		RL_GET_VALUE(spec, n * 3, &title);
		RL_SET_VALUE(blk, at++, title, RXT_STRING);

		wtype = RL_GET_VALUE(spec, n * 3 + 1, &w);
		if (wtype == RXT_INTEGER && w.int64 == 0) {
			RL_SET_VALUE(blk, at++, w, RXT_INTEGER);            // hidden
		} else if (n == last_shown && (wid->state & GUI_LIST_FILL)) {
			CLEARS(&w);
			RL_SET_VALUE(blk, at++, w, RXT_NONE);               // fills
		} else {
			REBINT now = Gui_List_Column_Width(wid, n);
			CLEARS(&w);
			w.int64 = (i64)(now < 0 ? 0 : now);
			RL_SET_VALUE(blk, at++, w, RXT_INTEGER);
		}

		RL_GET_VALUE(spec, n * 3 + 2, &a);
		if (RL_FIND_WORD(Gui_align_words, (REBCNT)a.int32a) != W_GUI_ALIGN_LEFT)
			RL_SET_VALUE(blk, at++, a, RXT_WORD);
	}
	RL_PROTECT_GC(blk, 0);
	return blk;
}

// FALSE when the cells do not make whole rows.
static REBOOL List_Set_Items(GUIWIDGET *wid, REBSER *blk, REBCNT index)
{
	REBCNT tail, cols = wid->fields;
	RXIARG val;

	if (blk) {
		tail = (REBCNT)RL_SERIES(blk, RXI_SER_TAIL);
		if (tail < index) index = tail;
		if (cols == 0 ? (tail > index) : ((tail - index) % cols != 0)) return FALSE;
		CLEARS(&val);
		val.series = blk;
		val.index  = index;
		Hob_Set_Payload(wid->hob, &val, RXT_BLOCK);
	} else {
		Hob_Set_Payload(wid->hob, NULL, RXT_NONE);
	}
	wid->picked = -1;
	Gui_List_Reload(wid, Gui_List_Rows(wid));
	return TRUE;
}


/***********************************************************************
**  tree-view: the nodes.
**
**  `items` is the menu dialect's grammar without the shortcuts: a label,
**  then optionally a word naming the node, then optionally a block of its
**  children in the same grammar.
**
**      ["Documents" docs ["Report.txt" report  "Old" ["a.txt"]]  "Music"]
**
**  It is parsed here, once, into a flat table (GUITREE - see gui.h),
**  parents before their children, and pushed at the backend a node at a
**  time. The block itself is kept in the payload slot, as a list-view's
**  is, and read back by `items`; the table holds copies of the labels,
**  so nothing in it points into Rebol memory.
**
**  A node is found by a PATH: a block of each node's word on the way
**  down, or its label where it has no word - `[docs old "a.txt"]`.
***********************************************************************/

static void Tree_Free(GUIWIDGET *wid)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBCNT   n;

	if (!tree) return;
	for (n = 0; n < tree->count; n++)
		if (tree->nodes[n].label) FREE_MEM(tree->nodes[n].label);
	if (tree->nodes) FREE_MEM(tree->nodes);
	FREE_MEM(tree);
	wid->tree = NULL;
}

// Appends a node under `parent` (-1 for the top); its index, or -1.
static REBINT Tree_Add(GUITREE *tree, REBINT parent, REBCNT word,
                       const REBYTE *label, REBCNT len)
{
	GUITREENODE *node;
	REBINT n, *link;

	if (tree->count == tree->capacity) {
		REBCNT cap = tree->capacity ? tree->capacity * 2 : 16;
		GUITREENODE *nodes = (GUITREENODE*)MAKE_MEM(sizeof(GUITREENODE) * cap);
		if (!nodes) return -1;
		if (tree->nodes) {
			COPY_MEM(nodes, tree->nodes, sizeof(GUITREENODE) * tree->count);
			FREE_MEM(tree->nodes);
		}
		tree->nodes = nodes;
		tree->capacity = cap;
	}

	n = (REBINT)tree->count;
	node = &tree->nodes[n];
	CLEARS(node);
	node->label = (REBYTE*)MAKE_MEM(len + 1);
	if (!node->label) return -1;
	if (len) COPY_MEM(node->label, label, len);
	node->label[len] = 0;
	node->len    = len;
	node->word   = word;
	node->parent = parent;
	node->first  = -1;
	node->next   = -1;
	tree->count++;

	// At the end of its parent's children, which keeps the dialect's order.
	if (parent >= 0) {
		link = &tree->nodes[parent].first;
		while (*link >= 0) link = &tree->nodes[*link].next;
		*link = n;
	} else {
		// After the last node at the top - the one with no next yet.
		REBINT k;
		for (k = 0; k < n; k++)
			if (tree->nodes[k].parent < 0 && tree->nodes[k].next < 0) {
				tree->nodes[k].next = n;
				break;
			}
	}
	return n;
}

// One level of the dialect, into `parent`. Anything which is not a label
// where one is expected is skipped, as in a menu.
static void Block_To_Tree(GUITREE *tree, REBSER *blk, REBCNT index, REBINT parent)
{
	REBCNT n, type;
	RXIARG val;

	for (n = index; (type = RL_GET_VALUE(blk, n, &val)) != 0; n++) {
		REBYTE *label = NULL;
		int     len;
		REBCNT  word = 0, next_type;
		RXIARG  next;
		REBINT  node;

		if (type == RXT_END) break;
		if (type != RXT_STRING) continue;
		len = RL_GET_UTF8_STRING((REBSER*)val.series, val.index, (void**)&label);
		if (len < 0) continue;

		next_type = RL_GET_VALUE(blk, n + 1, &next);
		if (next_type == RXT_WORD) {
			word = (REBCNT)next.int32a;
			n++;
			next_type = RL_GET_VALUE(blk, n + 1, &next);
		}
		node = Tree_Add(tree, parent, word, label, (REBCNT)len);
		if (node < 0) return;
		if (next_type == RXT_BLOCK) {
			Block_To_Tree(tree, (REBSER*)next.series, next.index, node);
			n++;
		}
	}
}

// Replaces the nodes; a NULL block leaves none.
static REBOOL Tree_Set_Items(GUIWIDGET *wid, REBSER *blk, REBCNT index)
{
	GUITREE *tree;
	RXIARG   val;
	REBCNT   n;

	Gui_Tree_Clear(wid);
	Tree_Free(wid);
	wid->picked = -1;

	if (!blk) {
		Hob_Set_Payload(wid->hob, NULL, RXT_NONE);
		Gui_Tree_End(wid);
		return TRUE;
	}

	tree = (GUITREE*)MAKE_MEM(sizeof(GUITREE));
	if (!tree) return FALSE;
	CLEARS(tree);
	wid->tree = tree;
	Block_To_Tree(tree, blk, index, -1);

	CLEARS(&val);
	val.series = blk;
	val.index  = index;
	Hob_Set_Payload(wid->hob, &val, RXT_BLOCK);

	// Parents come before their children in the table, so every parent's
	// native item exists by the time its children are added.
	for (n = 0; n < tree->count; n++) {
		GUITREENODE *node = &tree->nodes[n];
		if (node->parent >= 0 && !tree->nodes[node->parent].native) continue;
		node->native = Gui_Tree_Add_Node(wid, n);
	}
	Gui_Tree_End(wid);
	return TRUE;
}

// A node's name in a path: its word, or its label.
static void Tree_Id(GUITREENODE *node, RXIARG *val, REBCNT *type)
{
	CLEARS(val);
	if (node->word) {
		val->int32a = (i32)node->word;
		*type = RXT_WORD;
		return;
	}
	val->series = RL_DECODE_UTF_STRING(node->label, node->len, 8, FALSE, FALSE);
	val->index  = 0;
	*type = val->series ? RXT_STRING : RXT_NONE;
}

// The path to node `n`, as a new block. Deeper than 64 levels, the top
// of the path is left out - a tree that deep is not one a person reads.
#define TREE_MAX_DEPTH 64

static REBSER *Tree_Path(GUIWIDGET *wid, REBINT n)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBSER  *blk;
	REBINT   up[TREE_MAX_DEPTH], depth = 0, k;
	RXIARG   val;
	REBCNT   type;

	if (!tree || n < 0 || n >= (REBINT)tree->count) return NULL;
	for (k = n; k >= 0 && depth < TREE_MAX_DEPTH; k = tree->nodes[k].parent)
		up[depth++] = k;
	blk = (REBSER*)RL_MAKE_BLOCK((u32)depth);
	if (!blk) return NULL;

	// From the top down: a value is set by appending at the tail.
	RL_PROTECT_GC(blk, 1);
	for (k = 0; depth > 0; k++) {
		Tree_Id(&tree->nodes[up[--depth]], &val, &type);
		RL_SET_VALUE(blk, (u32)k, val, (int)type);
	}
	RL_PROTECT_GC(blk, 0);
	return blk;
}

/***********************************************************************
**  Node `n` as a script sees it: a node at the top is its word, or its
**  label, alone - a path of one would be `#(path! [docs])` - and any
**  other the path to it. FALSE when there is no such node.
***********************************************************************/
static REBOOL Tree_Value(GUIWIDGET *wid, REBINT n, RXIARG *val, REBCNT *type)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBSER  *path;

	CLEARS(val);
	*type = RXT_NONE;
	if (!tree || n < 0 || n >= (REBINT)tree->count) return FALSE;
	if (tree->nodes[n].parent < 0) {
		Tree_Id(&tree->nodes[n], val, type);
		return *type != RXT_NONE;
	}
	path = Tree_Path(wid, n);
	if (!path) return FALSE;
	val->series = path;
	val->index  = 0;
	*type = RXT_PATH;
	return TRUE;
}

// Whether a node is the one a word or a label names.
static REBOOL Tree_Is(GUITREENODE *node, REBCNT type, RXIARG *val)
{
	if (type == RXT_WORD) return node->word && node->word == (REBCNT)val->int32a;
	if (type == RXT_STRING) {
		REBYTE *utf8 = NULL;
		int len;
		if (node->word) return FALSE;   // a node with a word is named by it
		len = RL_GET_UTF8_STRING((REBSER*)val->series, val->index, (void**)&utf8);
		return len >= 0 && (REBCNT)len == node->len
		    && (len == 0 || memcmp(utf8, node->label, (size_t)len) == 0);
	}
	return FALSE;
}

/***********************************************************************
**  The node a value names: a path (or a block of the same) is followed
**  from the top. A word or a label alone is a node at the top when there
**  is one - it is what a top node reads back as - and otherwise the first
**  node anywhere with it, parents before children. -1 for none.
***********************************************************************/
static REBINT Tree_Find(GUIWIDGET *wid, REBCNT type, RXIARG *val)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBCNT   n;

	if (!tree || tree->count == 0) return -1;

	if (type == RXT_PATH || type == RXT_BLOCK) {
		REBSER *path = (REBSER*)val->series;
		REBINT  at = -1, child;
		RXIARG  step;
		REBCNT  t;
		for (n = val->index; (t = RL_GET_VALUE(path, n, &step)) != 0 && t != RXT_END; n++) {
			child = (at < 0) ? 0 : tree->nodes[at].first;
			if (at < 0) {
				// The top level: the nodes without a parent.
				REBCNT k;
				child = -1;
				for (k = 0; k < tree->count; k++)
					if (tree->nodes[k].parent < 0 && Tree_Is(&tree->nodes[k], t, &step)) {
						child = (REBINT)k;
						break;
					}
			} else {
				while (child >= 0 && !Tree_Is(&tree->nodes[child], t, &step))
					child = tree->nodes[child].next;
			}
			if (child < 0) return -1;
			at = child;
		}
		return at;
	}

	if (type == RXT_WORD || type == RXT_STRING) {
		for (n = 0; n < tree->count; n++)
			if (tree->nodes[n].parent < 0 && Tree_Is(&tree->nodes[n], type, val))
				return (REBINT)n;
		for (n = 0; n < tree->count; n++)
			if (Tree_Is(&tree->nodes[n], type, val)) return (REBINT)n;
	}
	return -1;
}

// Opens every branch above node `n`, so that it can be seen.
static void Tree_Open_To(GUIWIDGET *wid, REBINT n)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBINT   up[TREE_MAX_DEPTH], depth = 0, k;

	if (!tree || n < 0) return;
	for (k = tree->nodes[n].parent; k >= 0 && depth < TREE_MAX_DEPTH; k = tree->nodes[k].parent)
		up[depth++] = k;
	// From the top down, since a branch opens into its parent's.
	while (depth > 0) Gui_Tree_Expand(wid, (REBCNT)up[--depth], TRUE);
}

// Picks node `n` (-1: none) without it being reported.
static void Tree_Select(GUIWIDGET *wid, REBINT n)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	if (!tree || n >= (REBINT)tree->count) n = -1;
	Tree_Open_To(wid, n);
	wid->picked = n;
	Gui_Tree_Select(wid, n);
}

void Gui_Tree_Picked(GUIWIDGET *wid, REBINT n)
{
	REBINT x = 0, y = 0, w = 0, h = 0;

	if (!wid || n == wid->picked) return;
	wid->picked = n;
	if (!wid->hob) return;
	Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
	Gui_Queue_Event(wid->hob, EVT_CHANGE, x, y, 0);
}

// The open branches, each as a path, in the order of the nodes.
static REBSER *Tree_Expanded(GUIWIDGET *wid)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBSER  *blk;
	REBCNT   n, at = 0;
	RXIARG   val;

	blk = (REBSER*)RL_MAKE_BLOCK(4);
	if (!blk || !tree) return blk;
	RL_PROTECT_GC(blk, 1);
	for (n = 0; n < tree->count; n++) {
		REBCNT type;
		if (tree->nodes[n].first < 0) continue;   // a leaf opens nothing
		if (!Gui_Tree_Is_Expanded(wid, n)) continue;
		if (!Tree_Value(wid, (REBINT)n, &val, &type)) continue;
		RL_SET_VALUE(blk, at++, val, (int)type);
	}
	RL_PROTECT_GC(blk, 0);
	return blk;
}

// Every node, as a path, in the order of the table - which is what the
// `code` of an `open` or `close` event counts in.
static REBSER *Tree_Nodes(GUIWIDGET *wid)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBSER  *blk;
	REBCNT   n;
	RXIARG   val;

	blk = (REBSER*)RL_MAKE_BLOCK(tree ? tree->count : 0);
	if (!blk || !tree) return blk;
	RL_PROTECT_GC(blk, 1);
	for (n = 0; n < tree->count; n++) {
		REBCNT type;
		Tree_Value(wid, (REBINT)n, &val, &type);
		RL_SET_VALUE(blk, n, val, (int)type);
	}
	RL_PROTECT_GC(blk, 0);
	return blk;
}

void Gui_Tree_Toggled(GUIWIDGET *wid, REBINT n, REBOOL open)
{
	REBINT x = 0, y = 0, w = 0, h = 0;
	if (!wid || !wid->hob || n < 0) return;
	Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
	// 1-based, like every position a script sees.
	Gui_Queue_Event(wid->hob, open ? EVT_OPEN : EVT_CLOSE, x, y, n + 1);
}

// Opens exactly the branches named - with those above them - and closes
// the rest. Each is a path, or a word or label naming the first such node.
static void Tree_Set_Expanded(GUIWIDGET *wid, REBSER *blk, REBCNT index)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	REBCNT   n, type;
	RXIARG   val;

	if (!tree) return;
	// Deepest first, so a branch closes before the one holding it.
	for (n = tree->count; n-- > 0; )
		if (tree->nodes[n].first >= 0) Gui_Tree_Expand(wid, n, FALSE);
	for (n = index; (type = RL_GET_VALUE(blk, n, &val)) != 0 && type != RXT_END; n++) {
		REBINT node = Tree_Find(wid, type, &val);
		if (node < 0) continue;
		Tree_Open_To(wid, node);
		Gui_Tree_Expand(wid, (REBCNT)node, TRUE);
	}
	// Closing a branch may have taken the selection with it on some
	// platforms; what is picked now is not news to the script.
	wid->picked = Gui_Tree_Selected(wid);
}


// Appends a child's handle to its container's list.
static void Add_Child(REBHOB *parent, REBHOB *child)
{
	REBSER *kids;
	RXIARG  val;

	if (!parent || !child) return;
	kids = Hob_Children(parent, TRUE);
	if (!kids) return;

	Set_Handle_Arg(&val, child);
	RL_SET_VALUE(kids, (u32)RL_SERIES(kids, RXI_SER_TAIL), val, RXT_HANDLE);
}


// Takes it out again, keeping the order of the rest.
static void Drop_Child(REBHOB *parent, REBHOB *child)
{
	REBSER *kids;
	RXIARG  val;
	REBCNT  n, tail, kept = 0;

	if (!parent || !child) return;
	kids = Hob_Children(parent, FALSE);
	if (!kids) return;

	tail = (REBCNT)RL_SERIES(kids, RXI_SER_TAIL);
	for (n = 0; n < tail; n++) {
		if (RL_GET_VALUE(kids, n, &val) != RXT_HANDLE) continue;
		if (val.handle.hob == child) continue; // the one going away
		if (kept != n) RL_SET_VALUE(kids, kept, val, RXT_HANDLE);
		kept++;
	}

	// Nothing in the RL_ API shortens a block, and leaving the tail full
	// of stale handles is not an option - so the length is set directly
	// and the block re-terminated, which is what SET_VALUE would have
	// done on the way past.
	SERIES_TAIL(kids) = kept;
	SET_END(BLK_TAIL(kids));
}



/***********************************************************************
**  Called when a widget's native control is gone.
**
**  Unlinks it from its window, so that the window does not later try to
**  close a widget which has already been dealt with.
***********************************************************************/
void Gui_Widget_Closed(GUIWIDGET *widget)
{
	if (!widget) return;

	// Out of whatever held it - a container's own children block, or the
	// window's. Done before `parent` and `owner` are cleared below,
	// because they are what says where it was.
	Drop_Child(widget->parent
		? ((GUIWIDGET*)widget->parent)->hob
		: (widget->owner ? widget->owner->hob : NULL),
		widget->hob);

	if (widget->owner) {
		GUIWIDGET **link = (GUIWIDGET**)&widget->owner->widgets;
		while (*link) {
			if (*link == widget) { *link = (GUIWIDGET*)widget->next; break; }
			link = (GUIWIDGET**)&(*link)->next;
		}
		widget->owner = NULL;
	}
	widget->handle = NULL;
	widget->parent = NULL;
	widget->next   = NULL;
	Release_Handle(widget->hob);
}


/***********************************************************************
**  Empties a panel before it is itself destroyed.
**
**  Each child is destroyed properly rather than left to the panel: on
**  Windows the OS would take them anyway, but on macOS this file holds a
**  reference to every control, so letting the container drop them would
**  leak one object each. Destroying them first is right on both.
**
**  Immediate children only, and a nested panel is emptied before it goes
**  - so nothing is ever destroyed after the thing containing it, and a
**  native handle is always still valid when it is used.
**
**  The list is re-walked from the start after every removal, because
**  closing a widget unlinks it and any pointer into the list is stale
**  from that moment. These lists hold a handful of entries; correctness
**  is worth more here than a single pass.
***********************************************************************/
static void Close_Contents_Of(GUIWIDGET *panel)
{
	REBOOL again = TRUE;

	if (!panel || !panel->owner) return;

	while (again) {
		GUIWIDGET *wid;
		again = FALSE;
		for (wid = (GUIWIDGET*)panel->owner->widgets; wid;
		     wid = (GUIWIDGET*)wid->next)
		{
			if ((GUIWIDGET*)wid->parent != panel) continue;

			if (Kind_Is_Container(wid->kind)) Close_Contents_Of(wid);
			Gui_Destroy_Widget(wid); // the native control
			Gui_Widget_Closed(wid);  // the Rebol side of it
			again = TRUE;
			break;
		}
	}
}


/***********************************************************************
**  Called by the backend when a native window has really gone away.
**
**  Everything which kept the handle context alive is undone here, in one
**  place, so that it does not matter whether the window was closed by
**  `close-window`, by releasing the handle, or by the system.
**
**  Child controls go with their parent on both platforms, so their
**  handles are released here too - without destroying anything, which
**  the OS has already done. A backend which owns references to its
**  native child objects must have released them before calling this.
***********************************************************************/
void Gui_Window_Closed(REBHOB *window)
{
	GUIWIN *win;

	if (!window) return;

	win = (GUIWIN*)window->data;
	if (win) {
		GUIWIDGET *widget = (GUIWIDGET*)win->widgets;
		while (widget) {
			GUIWIDGET *next = (GUIWIDGET*)widget->next;
			widget->handle = NULL;
			widget->owner  = NULL;
			widget->next   = NULL;
			Release_Handle(widget->hob);
			widget = next;
		}
		win->widgets = NULL;
	}
	// Closed by the system rather than by us: off the stack all the same.
	if (win) Modal_Remove(win);
	if (Open_Windows > 0) Open_Windows--;
	Release_Handle(window);
}


//== helpers ==================================================================

// Validates argument `n` as a live window handle.
static GUIWIN* Frm_Window(RXIFRM *frm, REBCNT n)
{
	REBHOB *hob;
	if (!FRM_IS_HANDLE(n, Handle_GuiWindow)) return NULL;
	hob = RXA_HANDLE_CONTEXT(frm, n);
	if (!hob || !IS_USED_HOB(hob) || !hob->data) return NULL;
	return (GUIWIN*)hob->data;
}

// Which kinds carry a string: everything except the image widget. `text`
// means the label on a button or a static, and the contents of an entry.
static REBOOL Kind_Has_Text(REBCNT kind)
{
	return (kind == W_GUI_WIDGET_BUTTON
	     || kind == W_GUI_WIDGET_TEXT
	     || kind == W_GUI_WIDGET_FIELD
	     || kind == W_GUI_WIDGET_AREA
	     || kind == W_GUI_WIDGET_CHECK
	     || kind == W_GUI_WIDGET_RADIO
	     || kind == W_GUI_WIDGET_TOGGLE
	     // the caption of a framed panel; harmless on an unframed one,
	     // which simply keeps a string nothing draws
	     || kind == W_GUI_WIDGET_PANEL
	     // drop-list: readable, but not writable - see the set path.
	     // drop-down: readable AND writable - it is the typed value.
	     || kind == W_GUI_WIDGET_DROP_LIST
	     || kind == W_GUI_WIDGET_DROP_DOWN
	     || kind == W_GUI_WIDGET_TEXT_LIST
	     // the first cell of the picked row; read-only
	     || kind == W_GUI_WIDGET_LIST_VIEW
	     // the label of the tab shown; read-only, like a drop-list's
	     || kind == W_GUI_WIDGET_TAB_PANEL
	     // the label of the picked node; read-only
	     || kind == W_GUI_WIDGET_TREE_VIEW) ? TRUE : FALSE;
}

// Which kinds have a native border that `border?` and `/flat` switch off. A
// panel's frame is `border?` too, but drawn by the extension itself.
#define Kind_Has_Border(kind) \
	((kind) == W_GUI_WIDGET_FIELD || (kind) == W_GUI_WIDGET_AREA \
	 || (kind) == W_GUI_WIDGET_TEXT_LIST || (kind) == W_GUI_WIDGET_LIST_VIEW \
	 || (kind) == W_GUI_WIDGET_TREE_VIEW)

/***********************************************************************
**  A date! as it crosses the extension boundary (3.22.9 and later): the
**  32 bits of the core's REBYMD in `datetime.date`, and the time of day
**  in nanoseconds in `datetime.time` - NO_TIME for a date without one.
**
**  Unpacked by hand rather than through the REBYMD bit-fields: their
**  declared order depends on ENDIAN_LITTLE, which an extension build is
**  not guaranteed to define, while the NUMBER they make up is the same
**  either way - zone in bits 0-6, day 7-11, month 12-15, year 16-31.
***********************************************************************/
static void Date_From_Bits(u32 bits, GUIDATE *d)
{
	d->year  = (REBINT)(bits >> 16);
	d->month = (REBINT)((bits >> 12) & 0x0F);
	d->day   = (REBINT)((bits >> 7) & 0x1F);
	d->ns    = 0;
}

static u32 Bits_From_Date(const GUIDATE *d)
{
	// Zone 0: a date read back from a control is local and has none.
	return ((u32)d->year << 16) | (((u32)d->month & 0x0F) << 12)
	     | (((u32)d->day & 0x1F) << 7);
}

#define NS_PER_DAY ((REBI64)86400 * 1000000000)

/***********************************************************************
**  The date! a date-field is set to, into a GUIDATE. A date without a
**  time keeps the time of day the field has. (A date! never carries
**  24:00 or more; the modulo below only guards the backends against a
**  value that did.) A zone is not
**  applied: the field is in local time, and the date and time are read
**  as written. FALSE for a date the field cannot hold.
***********************************************************************/
static REBOOL Date_From_Arg(GUIWIDGET *wid, u32 bits, i64 time, GUIDATE *d)
{
	GUIDATE now;

	Date_From_Bits(bits, d);
	if (d->month < 1 || d->day < 1) return FALSE;
	if (time == NO_TIME) {
		if (Gui_Widget_Get_Date(wid, &now)) d->ns = now.ns;
	} else {
		d->ns = (REBI64)time % NS_PER_DAY;
		if (d->ns < 0) d->ns += NS_PER_DAY;
	}
	return TRUE;
}

// Which kinds hold a list of strings, and pick one of them by `index`.
#define Kind_Has_Items(kind) \
	((kind) == W_GUI_WIDGET_DROP_LIST || (kind) == W_GUI_WIDGET_DROP_DOWN \
	 || (kind) == W_GUI_WIDGET_TEXT_LIST || (kind) == W_GUI_WIDGET_TAB_PANEL \
	 || (kind) == W_GUI_WIDGET_LIST_VIEW)

// Which kinds are on or off.
static REBOOL Kind_Has_State(REBCNT kind)
{
	return (kind == W_GUI_WIDGET_CHECK
	     || kind == W_GUI_WIDGET_RADIO
	     || kind == W_GUI_WIDGET_TOGGLE) ? TRUE : FALSE;
}

/***********************************************************************
**  Which kinds the keyboard can reach.
**
**  Asked BEFORE the platform, because the platforms disagree: Win32's
**  SetFocus works on any enabled window, so it will happily focus a
**  progress bar or a label - the control takes the focus, shows nothing
**  and does nothing with a keystroke - while AppKit refuses both, since
**  neither accepts first responder.
**
**  A caller wants one answer, and the useful one is AppKit's: these are
**  not controls a user can reach, so `set-focus` refuses them and
**  `focused?` is false for them.
***********************************************************************/
static REBOOL Kind_Takes_Focus(REBCNT kind)
{
	return (kind == W_GUI_WIDGET_TEXT      // a label
	     || kind == W_GUI_WIDGET_IMAGE     // pixels, and a container
	     || kind == W_GUI_WIDGET_PANEL     // a container
	     || kind == W_GUI_WIDGET_LINE      // decoration
	     || kind == W_GUI_WIDGET_TAB_PANEL // a container; its tabs are reached with Tab
	     || kind == W_GUI_WIDGET_PROGRESS) // shows a value, takes no input
		? FALSE : TRUE;
}

// ... and which sit somewhere between 0% and 100%.
static REBOOL Kind_Has_Value(REBCNT kind)
{
	return (kind == W_GUI_WIDGET_SLIDER
	     || kind == W_GUI_WIDGET_PROGRESS) ? TRUE : FALSE;
}

/***********************************************************************
**  Block <-> list marshalling for a drop-list, a drop-down or a
**  text-list.
**
**  Done here rather than in the backends: it is identical work on every
**  platform, and a backend only has to know how to hold strings.
***********************************************************************/
static REBSER* Items_To_Block(GUIWIDGET *wid)
{
	REBCNT count = Gui_Widget_Count_Items(wid);
	REBCNT n;
	REBSER *blk;
	RXIARG item;

	// Sized up front, so filling it cannot expand - and so cannot collect
	// the strings put in on the way.
	blk = (REBSER*)RL_MAKE_BLOCK(count);
	if (!blk) return NULL;

	RL_PROTECT_GC(blk, 1);
	for (n = 0; n < count; n++) {
		REBSER *str = Gui_Widget_Get_Item(wid, n);
		if (!str) continue;
		CLEARS(&item);
		item.series = str;
		item.index  = 0;
		RL_SET_VALUE(blk, n, item, RXT_STRING);
	}
	RL_PROTECT_GC(blk, 0);
	return blk;
}


// Replaces the list. Anything in the block which is not a string is
// skipped rather than refused - a block of words or files is a reasonable
// thing to hand over, and FORM-ing it is the caller's business.
static void Block_To_Items(GUIWIDGET *wid, REBSER *blk)
{
	REBCNT n;
	REBCNT type;
	RXIARG val;

	Gui_Widget_Clear_Items(wid);
	if (!blk) return;

	for (n = 0; (type = RL_GET_VALUE(blk, n, &val)) != 0; n++) {
		REBYTE *utf8 = NULL;
		int len;

		if (type == RXT_END) break;
		if (type != RXT_STRING) continue;

		len = RL_GET_UTF8_STRING((REBSER*)val.series, val.index, (void**)&utf8);
		if (len < 0) continue;
		Gui_Widget_Add_Item(wid, utf8, (REBCNT)len);
	}
}


//== the menu dialect =========================================================
//
//   win/menu: [
//       "File" [
//           "New"     new  #"N"          ;; Ctrl+N / Cmd+N
//           "Save As" save [shift #"S"]  ;; ... with extra modifiers
//           ---                          ;; a dividing line
//           "Recent" ["Nothing yet" nil] ;; a block after a label: a submenu
//       ]
//   ]
//
// One item is a LABEL followed by a WORD, which is the id that comes back
// in the event - not the label, so that renaming "Save" does not break a
// handler. A label followed by a BLOCK is a submenu instead.
//
// The whole thing is parsed here and pushed at the backend through the
// Gui_Menu_* calls, so neither backend ever sees a Rebol value, and the
// dialect is defined exactly once.

// Item ids are 1-based indices into these two arrays, which grow together.
static REBOOL Menu_Add_Id(GUIWIN *win, REBCNT word)
{
	REBCNT  n = win->menu_count;
	REBCNT *ids;
	REBYTE *on;

	// Powers of two from 16: a menu bar is small, and this is built once.
	if ((n & (n - 1)) == 0 && n >= 16) {
		// n is a power of two and the arrays are exactly full
		ids = (REBCNT*)MAKE_MEM(sizeof(REBCNT) * n * 2);
		on  = (REBYTE*)MAKE_MEM(n * 2);
		if (!ids || !on) {
			if (ids) FREE_MEM(ids);
			if (on)  FREE_MEM(on);
			return FALSE;
		}
		COPY_MEM(ids, win->menu_ids, sizeof(REBCNT) * n);
		COPY_MEM(on,  win->menu_on,  n);
		FREE_MEM(win->menu_ids);
		FREE_MEM(win->menu_on);
		win->menu_ids = ids;
		win->menu_on  = on;
	} else if (n == 0) {
		win->menu_ids = (REBCNT*)MAKE_MEM(sizeof(REBCNT) * 16);
		win->menu_on  = (REBYTE*)MAKE_MEM(16);
		if (!win->menu_ids || !win->menu_on) return FALSE;
	}

	win->menu_ids[n] = word;
	win->menu_on[n]  = 1; // every item starts selectable
	win->menu_count  = n + 1;
	return TRUE;
}

static void Menu_Free_Ids(GUIWIN *win)
{
	if (win->menu_ids) FREE_MEM(win->menu_ids);
	if (win->menu_on)  FREE_MEM(win->menu_on);
	win->menu_ids   = NULL;
	win->menu_on    = NULL;
	win->menu_count = 0;
}

// A shortcut is either a bare char! - the platform's own menu modifier plus
// that key - or a block of modifier words ending in one.
static void Menu_Shortcut(REBCNT type, RXIARG *val, REBCNT *key, REBCNT *mods)
{
	*key  = 0;
	*mods = 0;

	if (type == RXT_CHAR) {
		*key = (REBCNT)val->int32a;
		return;
	}
	if (type != RXT_BLOCK) return;

	{	REBSER *blk = (REBSER*)val->series;
		REBCNT  n;
		RXIARG  item;
		REBCNT  t;

		for (n = val->index; (t = RL_GET_VALUE(blk, n, &item)) != 0; n++) {
			if (t == RXT_END) break;
			if (t == RXT_CHAR) { *key = (REBCNT)item.int32a; continue; }
			if (t != RXT_WORD) continue;
			switch (RL_FIND_WORD(Gui_menu_words, (REBCNT)item.int32a)) {
			case W_GUI_MENU_SHIFT:   *mods |= GUI_FLAG_SHIFT;   break;
			case W_GUI_MENU_CONTROL: *mods |= GUI_FLAG_CONTROL; break;
			case W_GUI_MENU_ALT:     *mods |= GUI_FLAG_ALT;     break;
			}
		}
	}
}

/***********************************************************************
**  Walks one level of the dialect, building into `parent` - which is
**  NULL for the menu bar itself and a backend's popup handle below it.
**
**  Recursive, because a submenu is the same grammar again. The depth is
**  whatever the caller wrote, and a block cannot contain itself, so
**  there is nothing to guard against.
***********************************************************************/
static void Block_To_Menu(GUIWIN *win, REBSER *blk, REBCNT index, void *parent)
{
	REBCNT n;
	REBCNT type;
	RXIARG val;

	if (!blk) return;

	for (n = index; (type = RL_GET_VALUE(blk, n, &val)) != 0; n++) {
		REBYTE *label = NULL;
		int     label_len;
		REBCNT  next_type;
		RXIARG  next;

		if (type == RXT_END) break;

		// `---` on its own
		if (type == RXT_WORD && (REBCNT)val.int32a == Word_Separator) {
			Gui_Menu_Add_Separator(win, parent);
			continue;
		}

		// Everything else starts with a label, and anything which is not
		// one is skipped rather than refused: a menu is a description, and
		// a stray value in it should not cost the caller the whole bar.
		if (type != RXT_STRING) continue;
		label_len = RL_GET_UTF8_STRING((REBSER*)val.series, val.index,
		                               (void**)&label);
		if (label_len < 0) continue;

		next_type = RL_GET_VALUE(blk, n + 1, &next);

		// label + block -> a submenu
		if (next_type == RXT_BLOCK) {
			void *popup = Gui_Menu_Add_Popup(win, parent, label,
			                                 (REBCNT)label_len);
			if (popup) {
				Block_To_Menu(win, (REBSER*)next.series, next.index, popup);
			}
			n++;
			continue;
		}

		// label + word -> an item, optionally followed by a shortcut
		if (next_type == RXT_WORD) {
			REBCNT key = 0, mods = 0;
			RXIARG after;
			REBCNT after_type;

			if (!Menu_Add_Id(win, (REBCNT)next.int32a)) return;

			after_type = RL_GET_VALUE(blk, n + 2, &after);
			if (after_type == RXT_CHAR || after_type == RXT_BLOCK) {
				Menu_Shortcut(after_type, &after, &key, &mods);
				n++;
			}
			Gui_Menu_Add_Item(win, parent, label, (REBCNT)label_len,
			                  win->menu_count, key, mods);
			n++;
			continue;
		}

		// A label with nothing after it is a dead item: it is shown, and
		// it does nothing, which is more informative than dropping it.
		if (Menu_Add_Id(win, 0)) {
			Gui_Menu_Add_Item(win, parent, label, (REBCNT)label_len,
			                  win->menu_count, 0, 0);
		}
	}
}


// Rebuilds the whole bar from a block, or takes it away when blk is NULL.
static REBOOL Set_Menu(GUIWIN *win, REBSER *blk, REBCNT index)
{
	Gui_Menu_Free(win);
	Menu_Free_Ids(win);
	// The PAYLOAD only: the same slot block holds the window's children,
	// which a menu going away has nothing to do with.
	Hob_Set_Payload(win->hob, NULL, RXT_NONE);

	if (!blk) return TRUE;

	if (!Gui_Menu_Begin(win)) return FALSE;
	Block_To_Menu(win, blk, index, NULL);
	if (!Gui_Menu_End(win)) {
		Gui_Menu_Free(win);
		Menu_Free_Ids(win);
		return FALSE;
	}

	// Kept so that `win/menu` can answer with the very block it was given,
	// in the payload half of the one slot the GC marks.
	{	RXIARG val;
		CLEARS(&val);
		val.series = blk;
		val.index  = index;
		Hob_Set_Payload(win->hob, &val, RXT_BLOCK);
	}
	return TRUE;
}


/***********************************************************************
**  Called by a backend when an item is picked.
**
**  The native id is only ever an index into this window's table; the
**  WORD is what reaches Rebol, which is why renaming a label cannot
**  break a handler.
***********************************************************************/
/***********************************************************************
**  The system switched between the light and the dark appearance.
**
**  Reported as `theme-change` with the window as the source and `light`
**  or `dark` in `code`, and only when the window's appearance really is
**  different from what was last reported: Windows sends its settings
**  broadcast several times for one switch, and for plenty of changes
**  which are not this one.
***********************************************************************/
void Gui_Theme_Changed(GUIWIN *win, REBOOL dark)
{
	REBOOL was;

	if (!win || !win->hob) return;
	was = (win->flags & GUIW_DARK) ? TRUE : FALSE;
	if (was == (dark ? TRUE : FALSE)) return;

	if (dark) win->flags |=  GUIW_DARK;
	else      win->flags &= ~(REBCNT)GUIW_DARK;

	Gui_Queue_Event(win->hob, EVT_THEME_CHANGE, 0, 0,
		(REBINT)Gui_theme_words[dark ? W_GUI_THEME_DARK : W_GUI_THEME_LIGHT]);
}


void Gui_Menu_Picked(GUIWIN *win, REBCNT id)
{
	if (!win || !win->hob) return;
	if (id == 0 || id > win->menu_count) return;
	if (win->menu_ids[id - 1] == 0) return; // a label with no id of its own

	Gui_Queue_Event(win->hob, EVT_MENU_SELECT, 0, 0,
	                (REBINT)win->menu_ids[id - 1]);
}


// An image is painted, not operated, and a progress bar takes no input at
// all - neither has an enabled state worth reporting.
static REBOOL Kind_Has_Enabled(REBCNT kind)
{
	// A panel is left out because the two platforms disagree: disabling a
	// child window on Windows greys everything inside it, while an NSView
	// has no enabled state at all.
	return (kind != W_GUI_WIDGET_IMAGE
	     && kind != W_GUI_WIDGET_PROGRESS
	     && kind != W_GUI_WIDGET_LINE      // decoration, nothing to operate
	     && kind != W_GUI_WIDGET_TAB_PANEL // a container, as a panel
	     && kind != W_GUI_WIDGET_PANEL) ? TRUE : FALSE;
}

// Which kinds can be read-only: the two the user types into. A label
// cannot be edited to begin with, and a drop-list's text is already
// read-only in a sense of its own - you pick an item rather than write one.
#define Kind_Has_Read_Only(kind) \
	((kind) == W_GUI_WIDGET_FIELD || (kind) == W_GUI_WIDGET_AREA)

// Which kinds scroll, and can be asked where they are. A drop-list's or a
// drop-down's list scrolls too, but is not addressable.
#define Kind_Scrolls(kind) \
	((kind) == W_GUI_WIDGET_AREA || (kind) == W_GUI_WIDGET_TEXT_LIST \
	 || (kind) == W_GUI_WIDGET_LIST_VIEW || (kind) == W_GUI_WIDGET_TREE_VIEW)

static const char* Kind_Name(REBCNT kind)
{
	switch (kind) {
	case W_GUI_WIDGET_IMAGE: return "image";
	case W_GUI_WIDGET_TEXT:  return "text";
	case W_GUI_WIDGET_FIELD: return "field";
	case W_GUI_WIDGET_AREA:  return "area";
	case W_GUI_WIDGET_CHECK: return "check";
	case W_GUI_WIDGET_RADIO: return "radio";
	case W_GUI_WIDGET_TOGGLE: return "toggle";
	case W_GUI_WIDGET_SLIDER:   return "slider";
	case W_GUI_WIDGET_PROGRESS: return "progress";
	case W_GUI_WIDGET_DROP_LIST: return "drop-list";
	case W_GUI_WIDGET_DROP_DOWN: return "drop-down";
	case W_GUI_WIDGET_TEXT_LIST: return "text-list";
	case W_GUI_WIDGET_DATE_FIELD: return "date-field";
	case W_GUI_WIDGET_PANEL:     return "panel";
	case W_GUI_WIDGET_LINE:      return "line";
	case W_GUI_WIDGET_TAB_PANEL: return "tab-panel";
	case W_GUI_WIDGET_LIST_VIEW: return "list-view";
	case W_GUI_WIDGET_TREE_VIEW: return "tree-view";
	default:                 return "button";
	}
}

// ... and as a live widget handle.
static GUIWIDGET* Frm_Widget(RXIFRM *frm, REBCNT n)
{
	REBHOB *hob;
	if (!FRM_IS_HANDLE(n, Handle_GuiWidget)) return NULL;
	hob = RXA_HANDLE_CONTEXT(frm, n);
	if (!hob || !IS_USED_HOB(hob) || !hob->data) return NULL;
	return (GUIWIDGET*)hob->data;
}

/***********************************************************************
**  Resolves argument `n` as the thing a new widget goes into: either a
**  window handle, or a panel handle.
**
**  Returns the owning WINDOW, and sets *panel to the containing panel or
**  NULL. Widgets always belong to a window - the panel only says where
**  they sit inside it - which is what keeps the window's widget list flat
**  and teardown a single walk.
***********************************************************************/
static GUIWIN* Frm_Parent(RXIFRM *frm, REBCNT n, GUIWIDGET **container)
{
	GUIWIDGET *wid;
	GUIWIN *win;

	*container = NULL;

	win = Frm_Window(frm, n);
	if (win) return win;

	wid = Frm_Widget(frm, n);
	if (!wid || !wid->handle) return NULL;

	// Which kinds may hold other widgets. A PANEL is the obvious one; an
	// IMAGE is here so that a label or a check can sit ON rendered pixels
	// rather than beside them - which only works if it is a CHILD of the
	// image, because two overlapping siblings have no defined order on
	// Win32 and would fight over the same pixels.
	//
	// Nothing else: a button with children inside it is not a thing, and
	// the native control would not clip or move them.
	if (wid->kind != W_GUI_WIDGET_PANEL && wid->kind != W_GUI_WIDGET_IMAGE)
		return NULL;

	*container = wid;
	return wid->owner;
}


/***********************************************************************
**  The last step of every add-* command.
**
**  Links the widget onto its window's list - done here rather than by a
**  backend, so the list has exactly one owner and both platforms behave
**  the same - locks the handle against the GC, and paints the control.
**
**  The redraw request matters: a control created after the window was
**  shown has nothing on screen until something paints it.
**
**  What that request DOES is the backend's business, and the two differ.
**  Windows paints there and then. macOS only marks the control and lets
**  the next Gui_Pump() paint it, because AppKit draws between events and
**  forcing it at any other moment is unreliable - the first widgets of a
**  batch would silently never appear.
***********************************************************************/
// Which kinds have text to set a font and a colour on. It is exactly the
// kinds which have `text` - if there is nothing to read, there is nothing
// to style - so the two questions share one answer rather than drifting
// apart as kinds are added.
// ... except a tab-panel, whose text is only the label of a tab: its
// font, colours and background are not something both platforms let a
// script change.
#define Kind_Has_Font(kind) \
	(Kind_Has_Text(kind) && (kind) != W_GUI_WIDGET_TAB_PANEL)

/***********************************************************************
**  Gives a widget the size its own content asks for, on the axes named.
**
**  ONLY WHEN ASKED. Nothing here happens on its own: a widget which was
**  told how big to be stays that size, whatever happens to its font or
**  its text afterwards, because the box a script laid out is the box it
**  meant. Re-fitting on every change would move widgets around under a
**  layout that had already been settled.
**
**  Asking is a zero axis - the convention `add-*` already uses, both at
**  creation and now through `widget/size:`.
**
**  Returns FALSE for a kind with nothing to measure: an image widget is
**  whatever size it was given, and a panel is a container whose contents
**  this layer knows nothing about.
***********************************************************************/
static REBOOL Fit_To_Content(GUIWIDGET *wid, REBOOL fit_w, REBOOL fit_h)
{
	REBINT nw = 0, nh = 0;
	REBINT x, y, w, h;

	if (!wid || (!fit_w && !fit_h)) return TRUE; // nothing asked for
	if (!Gui_Widget_Natural_Size(wid, &nw, &nh)) return FALSE;
	if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) return FALSE;

	if (fit_w && nw > 0) w = nw;
	if (fit_h && nh > 0) h = nh;

	Gui_Widget_Set_Box(wid, x, y, w, h);
	return TRUE;
}

/***********************************************************************
**  Links a new widget to its window, and finishes it off.
**
**  `w` and `h` are what the caller ASKED for; a zero in either means
**  "work it out", and this is where that is worked out - after the
**  font, because the answer depends on it, and before the first draw,
**  so nothing is ever seen at the placeholder size.
***********************************************************************/
static void Attach_Widget(GUIWIDGET *wid, GUIWIN *win, REBINT w, REBINT h)
{
	wid->next = win->widgets;
	win->widgets = wid;

	if (wid->hob) wid->hob->flags |= HANDLE_CONTEXT_LOCKED;

	// And onto whatever holds it, which is what `children` reads back.
	// The intrusive list above is the extension's own and window-wide;
	// this is the per-container one Rebol sees.
	Add_Child(wid->parent ? ((GUIWIDGET*)wid->parent)->hob : win->hob,
	          wid->hob);

	// The window's default is read HERE, once, at creation - which is the
	// whole of the inheritance. Restyling a window afterwards changes what
	// the next widget starts with and leaves everything already on screen
	// alone, so a widget's font is only ever its own business.
	if (Kind_Has_Font(wid->kind)
	    && (win->font.name || win->font.size || win->font.style)) {
		Gui_Widget_Set_Font(wid,
			(const REBYTE*)win->font.name,
			win->font.name ? (REBCNT)strlen(win->font.name) : 0,
			win->font.size, win->font.style);
	}

	// A zero axis asks the widget, once, here. A kind with nothing to
	// measure keeps the placeholder box it was created at, which is why
	// the answer is not checked: `add-panel win 20x20 0x0` is the caller
	// asking a container how big its contents are, and there is no answer
	// to that at this level.
	Fit_To_Content(wid, w <= 0 ? TRUE : FALSE, h <= 0 ? TRUE : FALSE);

	// Invalidate, do not paint. A script builds its whole layout with
	// nothing pumping in between, so painting here would make the window
	// assemble itself visibly, one widget per `add-*`. Left to the pump,
	// every widget added since the last one appears together.
	Gui_Widget_Invalidate(wid);
}


/***********************************************************************
**  A window's default font, which is the only GUIFONT anything keeps.
**
**  The name is a copy: the Rebol string it came from belongs to the
**  caller and may be modified or collected the moment the accessor
**  returns.
***********************************************************************/
static REBOOL Font_Set_Name(GUIFONT *font, const REBYTE *utf8, REBCNT len)
{
	char *copy = NULL;

	if (utf8 && len > 0) {
		copy = (char*)MAKE_MEM(len + 1);
		if (!copy) return FALSE;
		COPY_MEM(copy, utf8, len);
		copy[len] = 0;
	}
	if (font->name) FREE_MEM(font->name);
	font->name = copy;
	return TRUE;
}

static void Font_Free(GUIFONT *font)
{
	if (font->name) FREE_MEM(font->name);
	font->name = NULL;
	font->size = 0;
	font->style = 0;
}


/***********************************************************************
**  Changing one part of a widget's font.
**
**  A native font is one object, not four settings, so every one of the
**  four accessors is the same read-change-write. `part` says which of
**  them is being written; the rest are carried over from what the
**  control already has.
***********************************************************************/
enum gui_font_part { FONT_PART_NAME, FONT_PART_SIZE, FONT_PART_STYLE };

static REBOOL Set_Font_Part(GUIWIDGET *wid, REBCNT part,
                            const REBYTE *name, REBCNT name_len,
                            REBINT size, REBCNT style, REBCNT style_mask)
{
	REBSER *current_name = NULL;
	REBINT  current_size = 0;
	REBCNT  current_style = 0;
	REBYTE *utf8 = NULL;
	int     len;

	if (!Gui_Widget_Get_Font(wid, &current_name, &current_size, &current_style))
		return FALSE;

	// Whatever the caller is not writing comes back from the control.
	if (part != FONT_PART_NAME && current_name) {
		len = RL_GET_UTF8_STRING(current_name, 0, (void**)&utf8);
		if (len > 0) {
			name     = utf8;
			name_len = (REBCNT)len;
		}
	}
	if (part != FONT_PART_SIZE) size = current_size;
	if (part != FONT_PART_STYLE)
		style = current_style;
	else
		style = (current_style & ~style_mask) | (style & style_mask);

	// Deliberately no re-fit here. A bigger font in a box which was laid
	// out for a smaller one clips, and the remedy is `widget/size:` with a
	// zero axis - a decision for the script, which knows what else is
	// around the widget, rather than for this function.
	return Gui_Widget_Set_Font(wid, name, name_len, size, style);
}


// Fills an RXIARG with a handle context, as `poll-events` and the `parent`
// accessor both need to hand one back to Rebol.
static void Set_Handle_Arg(RXIARG *arg, REBHOB *hob)
{
	CLEARS(arg);
	arg->handle.hob   = hob;
	arg->handle.type  = hob->sym;
	arg->handle.flags = hob->flags;
	arg->handle.index = hob->index;
}


/***********************************************************************
**  The image behind an image widget, as the backends want it.
**
**  Read fresh on every paint rather than cached: the series can move
**  when it is expanded, and its dimensions can change under the widget,
**  so a stored pointer would eventually be a stale one.
**
**  The pixel order is the image! datatype's own - BGRA on both supported
**  platforms - so neither backend converts anything.
***********************************************************************/
REBOOL Gui_Widget_Pixels(GUIWIDGET *wid, REBYTE **data, REBINT *w, REBINT *h)
{
	REBSER *img;

	if (!wid || !wid->hob) return FALSE;
	img = Hob_Payload(wid->hob);
	if (!img) return FALSE;

	*w    = (REBINT)IMG_WIDE(img);
	*h    = (REBINT)IMG_HIGH(img);
	*data = IMG_DATA(img);

	return (*w > 0 && *h > 0 && *data) ? TRUE : FALSE;
}


//== commands =================================================================

/***********************************************************************
**  open-window size [pair!] /title text [string!] /at offset [pair!] /hidden
**
**  `size` is the CLIENT size - the frame is added on top of it, so the
**  window has room for exactly the requested number of pixels.
***********************************************************************/
COMMAND cmd_gui_open_window(RXIFRM *frm, void *ctx)
{
	REBHOB *hob;
	GUIWIN *win;
	REBINT  x = GUI_DEFAULT_POS, y = GUI_DEFAULT_POS;
	REBINT  w = (REBINT)RXA_PAIR(frm, 1).x;
	REBINT  h = (REBINT)RXA_PAIR(frm, 1).y;
	REBYTE *title = NULL;
	REBCNT  title_len = 0;
	REBCNT  flags = 0;

	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	if (RXA_REF(frm, 2)) { // /title
		int len = RL_GET_UTF8_STRING(RXA_SERIES(frm, 3), RXA_INDEX(frm, 3), (void**)&title);
		if (len > 0) title_len = (REBCNT)len;
	}
	if (RXA_REF(frm, 4)) { // /at
		x = (REBINT)RXA_PAIR(frm, 5).x;
		y = (REBINT)RXA_PAIR(frm, 5).y;
	}
	if (RXA_REF(frm, 7)) flags |= GUI_WIN_FIXED;       // /fixed
	if (RXA_REF(frm, 8)) flags |= GUI_WIN_BORDERLESS;  // /borderless
	if (RXA_REF(frm, 9)) flags |= GUI_WIN_TRANSPARENT; // /transparent

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWindow);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	win = (GUIWIN*)hob->data;
	win->hob = hob; // the window procedure tags its events with it

	/*******************************************************************
	**  `/modal owner`: centred on the owner unless placed, and kept above
	**  it by the backend. The window is created before it joins the
	**  stack, so nothing is blocked if it fails.
	*******************************************************************/
	if (RXA_REF(frm, 10)) {
		GUIWIN *owner = Frm_Window(frm, 11);
		REBINT  ox, oy, ow, oh;
		if (!owner || !owner->handle) {
			RL_FREE_HANDLE_CONTEXT(hob);
			RETURN_ERROR(ERR_INVALID_HANDLE);
		}
		if (Modal_Depth >= GUI_MODAL_MAX) {
			RL_FREE_HANDLE_CONTEXT(hob);
			RETURN_ERROR(ERR_MODAL_DEPTH);
		}
		win->modal_owner = owner;
		if (!RXA_REF(frm, 4) && Gui_Get_Offset(owner, &ox, &oy) && Gui_Get_Size(owner, &ow, &oh)) {
			x = ox + (ow - w) / 2;
			y = oy + (oh - h) / 2;
		}
	}

	if (!Gui_Open_Window(win, x, y, w, h, title, title_len, flags)) {
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WINDOW);
	}

	// What it opened in, so that the first `theme-change` is a real change.
	if (Gui_Window_Dark(win)) win->flags |= GUIW_DARK;

	// `border?`: on, unless the window was asked for with nothing around it -
	// `/borderless` - which is what it has always meant.
	// A see-through window starts without one too: an outline and a shadow
	// round a client area that is not there only frame a hole.
	if (!(flags & (GUI_WIN_BORDERLESS | GUI_WIN_TRANSPARENT))) win->flags |= GUIW_BORDER;
	Gui_Window_Apply_Border(win);

	// The native window and every queued event point back at this context,
	// so the GC must leave it alone until the window is closed.
	hob->flags |= HANDLE_CONTEXT_LOCKED;

	Open_Windows++; // the device poll pumps only while one of these exists

	if (win->modal_owner) {
		win->flags |= GUIW_MODAL;
		Modal_Stack[Modal_Depth++] = win;
		Gui_Apply_Modal(win);
	}

	if (!RXA_REF(frm, 6)) Gui_Show_Window(win, TRUE); // /hidden

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  gui-device / gui-device-polls
**
**  Diagnostics, and the only way a script can tell from Rebol that the
**  device was accepted and is being polled - which is the thing the
**  whole event model now rests on.
***********************************************************************/
COMMAND cmd_gui_gui_device(RXIFRM *frm, void *ctx)
{
	RXA_INT64(frm, 1) = (i64)Gui_Dev_Id;
	RXA_TYPE(frm, 1) = RXT_INTEGER;
	return RXR_VALUE;
}

COMMAND cmd_gui_gui_device_polls(RXIFRM *frm, void *ctx)
{
	RXA_INT64(frm, 1) = (i64)Gui_Dev_Polls;
	RXA_TYPE(frm, 1) = RXT_INTEGER;
	return RXR_VALUE;
}

COMMAND cmd_gui_gui_device_events(RXIFRM *frm, void *ctx)
{
	RXA_INT64(frm, 1) = (i64)Gui_Dev_Events;
	RXA_TYPE(frm, 1) = RXT_INTEGER;
	return RXR_VALUE;
}

COMMAND cmd_gui_gui_device_pumps(RXIFRM *frm, void *ctx)
{
	RXA_INT64(frm, 1) = (i64)Gui_Dev_Pumps;
	RXA_TYPE(frm, 1) = RXT_INTEGER;
	return RXR_VALUE;
}

COMMAND cmd_gui_gui_device_messages(RXIFRM *frm, void *ctx)
{
	RXA_INT64(frm, 1) = (i64)Gui_Dev_Msgs;
	RXA_TYPE(frm, 1) = RXT_INTEGER;
	return RXR_VALUE;
}


/***********************************************************************
**  gui-port-open / gui-port-close / gui-port-read
**      port [port!]
**
**  The three actors of the `gui` scheme, which exists so that the device
**  has somewhere to deliver the event that wakes WAIT. They are NOT
**  exported: the scheme is defined in this extension's own mezzanine and
**  is the only caller.
**
**  Each one hands the port's request to the device's command table, so a
**  failure here means the device refused - not that the command is a
**  polite no-op.
***********************************************************************/
COMMAND cmd_gui_gui_port_open(RXIFRM *frm, void *ctx)
{
	REBREQ *req = GUI_DEV_REQ(1);
	if (!req) RETURN_ERROR(ERR_NO_PORT_STATE);
	if (RL_DO_DEVICE(req, RDC_OPEN) < 0) RETURN_ERROR(ERR_DEVICE_FAIL);
	return RXR_VALUE; // the port, unchanged
}

COMMAND cmd_gui_gui_port_close(RXIFRM *frm, void *ctx)
{
	REBREQ *req = GUI_DEV_REQ(1);
	if (!req) RETURN_ERROR(ERR_NO_PORT_STATE);
	if (RL_DO_DEVICE(req, RDC_CLOSE) < 0) RETURN_ERROR(ERR_DEVICE_FAIL);
	return RXR_VALUE;
}

// Reports how many events `poll-events` would return, and drains nothing.
COMMAND cmd_gui_gui_port_read(RXIFRM *frm, void *ctx)
{
	REBREQ *req = GUI_DEV_REQ(1);
	if (!req) RETURN_ERROR(ERR_NO_PORT_STATE);
	if (RL_DO_DEVICE(req, RDC_READ) < 0) RETURN_ERROR(ERR_DEVICE_FAIL);
	RXA_INT64(frm, 1) = (i64)req->actual;
	RXA_TYPE(frm, 1) = RXT_INTEGER;
	return RXR_VALUE;
}


/***********************************************************************
**  close-window window [handle!]
**
**  Destroys the native window. The handle stays valid and simply reports
**  `open?` as false afterwards, so Rebol code holding it cannot crash.
***********************************************************************/
COMMAND cmd_gui_close_window(RXIFRM *frm, void *ctx)
{
	GUIWIN *win = Frm_Window(frm, 1);

	if (!win) RETURN_ERROR(ERR_INVALID_HANDLE);

	// Closing an already closed window is not an error - it is what a
	// `close` event handler ends up doing when the user was quicker.
	if (!win->handle) return RXR_TRUE;

	// Not while a dialog is open on it: the dialog is waiting for an
	// answer about this very window.
	if (Has_Modal(win)) RETURN_ERROR(ERR_HAS_MODAL);

	Modal_Remove(win);       // input back to the rest BEFORE it goes
	Gui_Close_Window(win);   // -> Gui_Window_Closed()

	return RXR_TRUE;
}


COMMAND cmd_gui_show_window(RXIFRM *frm, void *ctx)
{
	GUIWIN *win = Frm_Window(frm, 1);
	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
	Gui_Show_Window(win, TRUE);
	return RXR_TRUE;
}


COMMAND cmd_gui_hide_window(RXIFRM *frm, void *ctx)
{
	GUIWIN *win = Frm_Window(frm, 1);
	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
	Gui_Show_Window(win, FALSE);
	return RXR_TRUE;
}


/***********************************************************************
**  Turns a queued drop payload into a drop HANDLE.
**
**  Called only from `poll-events`, which is on the interpreter's own
**  thread of control - the producer side must not allocate, which is
**  the whole reason the payload is plain C memory until here.
**
**  Everything the handle reports lives in its shared hob->series slot:
**
**      [0] the content - a block of file! for files, a string! for text
**      [1] the target  - the window or widget handle it was dropped on
**
**  Both are therefore marked by the collector, so a drop handle kept by
**  a script stays valid however long it is held, and the target cannot
**  dangle after its window closes - it reports itself as closed, the
**  same as any other widget handle would.
**
**  Returns NULL if anything could not be allocated; the caller then
**  reports the event with its target as the source rather than dropping
**  it, so a drop is never silently lost.
***********************************************************************/
static REBHOB *Make_Drop_Handle(REBHOB *target, GUIDROPDATA *data)
{
	REBHOB  *hob;
	GUIDROP *drop;
	REBSER  *slots;
	REBSER  *content;
	RXIARG   val;
	REBCNT   n;
	REBYTE  *at;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiDrop);
	if (!hob) return NULL;

	// The slot block first, and attached to the handle BEFORE it is filled:
	// anything allocated after this point is reachable from the handle, so a
	// collection in the middle of filling it cannot take half of it away.
	slots = (REBSER*)RL_MAKE_BLOCK(2);
	if (!slots) return NULL;
	hob->series = slots;
	CLEARS(&val);
	RL_SET_VALUE(slots, 0, val, RXT_NONE);
	RL_SET_VALUE(slots, 1, val, RXT_NONE);

	drop = (GUIDROP*)hob->data;
	drop->hob   = hob;
	drop->kind  = data->kind;
	drop->count = data->count;

	if (data->kind == GUI_DROP_TEXT) {
		// One string, however many NULs the payload happens to hold.
		content = RL_DECODE_UTF_STRING(data->text, data->size ? data->size - 1 : 0,
		                               8, FALSE, FALSE);
		if (!content) return NULL;
		CLEARS(&val);
		val.series = content;
		val.index  = 0;
		RL_SET_VALUE(slots, 0, val, RXT_STRING);
	}
	else {
		// A block of file!, converted from the local path form. The block is
		// stored EMPTY first, for the same reason the slot block is attached
		// early: RL_TO_REBOL_PATH allocates, and the block has to be
		// reachable before it does.
		content = (REBSER*)RL_MAKE_BLOCK(data->count);
		if (!content) return NULL;
		CLEARS(&val);
		val.series = content;
		val.index  = 0;
		RL_SET_VALUE(slots, 0, val, RXT_BLOCK);

		at = data->text;
		for (n = 0; n < data->count; n++) {
			REBCNT len = (REBCNT)LEN_BYTES(at);
			REBSER *path = RL_TO_REBOL_PATH(at, len, 0);
			if (path) {
				CLEARS(&val);
				val.series = path;
				val.index  = 0;
				RL_SET_VALUE(content, n, val, RXT_FILE);
			}
			at += len + 1;
		}
	}

	// The target, so that a handler knows what it was dropped ON without
	// having to remember what it was hovering over.
	Set_Handle_Arg(&val, target);
	RL_SET_VALUE(slots, 1, val, RXT_HANDLE);

	return hob;
}


/***********************************************************************
**  The modifiers of a queued event, as the event!'s own flag bits.
**
**  GUI_FLAG_* is what the backends report; EVF_* is what `evt/flags`
**  reads back as a block of words. They are separate enums on purpose -
**  the backends must not have to know the core's bit numbering.
***********************************************************************/
static REBYTE Event_Modifier_Bits(REBINT value)
{
	REBYTE bits = 0;
	if (value & GUI_FLAG_SHIFT)   bits |= (1 << EVF_SHIFT);
	if (value & GUI_FLAG_CONTROL) bits |= (1 << EVF_CONTROL);
	if (value & GUI_FLAG_ALT)     bits |= (1 << EVF_ALT);
	if (value & GUI_FLAG_DOUBLE)  bits |= (1 << EVF_DOUBLE);
	return bits;
}


/***********************************************************************
**  poll-events
**
**  Dispatches everything the OS has waiting - which is what fills the
**  queue - and returns the collected events as a block of event!
**  values:
**
**      foreach evt poll-events [switch evt/type [...]]
**
**  Always returns a block, empty when nothing happened, so the caller
**  never has to test before iterating.
**
**  WHY event! rather than the flat four-value records this used to
**  return: the arity was fixed at the call site, so the day a GUI event
**  needed a fifth piece of information every `foreach [type source
**  position value]` in existence would have started reading the next
**  event's type as its own value - silently. An event! grows a field
**  instead. It also costs one REBVAL per event rather than four, and it
**  is what every other event source in Rebol already speaks, so one
**  handler can take a GUI event and a port event through one path.
**
**  The types are the core's own EVT_* codes, so a script switches on
**  the same words every other event source in Rebol reports - there is
**  no event vocabulary of this extension's own to learn.
**
**  What each field carries:
**
**      type     `click`, `change`, `scroll-line`, `menu-select`, ...
**      source   what produced it: a window, or the widget itself for
**               `click`, `change`, `focus` and `unfocus`
**      offset   client coordinates; the new client SIZE for `resize`
**      flags    shift / control / alt / double, where they apply
**      code     the wheel delta in lines, or a menu item's WORD
**
**  `offset` and `code` are the two readings of the event's one payload
**  word, so an event has one or the other and never both - which is why
**  a wheel event reports no position. The widget it happened to is in
**  `source`, which is the part anyone actually switches on.
***********************************************************************/
/***********************************************************************
**  set-focus
**      target [handle!]
**
**  A widget, or a window to focus the window itself. Answers FALSE when
**  the target cannot take the focus - a label, a progress bar, a
**  disabled or closed control - rather than pretending it worked.
***********************************************************************/
COMMAND cmd_gui_set_focus(RXIFRM *frm, void *ctx)
{
	if (FRM_IS_HANDLE(1, Handle_GuiWindow)) {
		GUIWIN *win = Frm_Window(frm, 1);
		if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
		return Gui_Window_Set_Focus(win) ? RXR_TRUE : RXR_FALSE;
	}
	{
		GUIWIDGET *wid = Frm_Widget(frm, 1);
		if (!wid || !wid->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
		if (!Kind_Takes_Focus(wid->kind)) return RXR_FALSE;
		return Gui_Widget_Set_Focus(wid) ? RXR_TRUE : RXR_FALSE;
	}
}


COMMAND cmd_gui_poll_events(RXIFRM *frm, void *ctx)
{
	REBSER *blk;
	REBCNT  count, n, at = 0;
	RXIARG  val;
	REBEVT  ev;

	/*******************************************************************
	**  Pumps and drains, and never sleeps.
	**
	**  Sleeping is WAIT's job: the extension registers a device with
	**  RDO_AUTO_POLL, so the host pumps this same queue from inside
	**  OS_Wait and reports pending events back to it. That is what lets
	**  one sleep serve both queues - see Poll_Gui() in gui.c.
	*******************************************************************/
	Gui_Pump();
	Gui_Track_Pointer();

	if (Event_Dropped) {
		printf("GUI: dropped %u events (queue full)\n", Event_Dropped);
		Event_Dropped = 0;
	}

	count = QUEUE_COUNT();
	blk = (REBSER*)RL_MAKE_BLOCK(count);
	if (!blk) RETURN_ERROR(ERR_NO_HANDLE);

	// RL_Set_Value may expand the block, and an expansion can collect - so
	// the series is protected until it is safely stored in the frame.
	RL_PROTECT_GC(blk, 1);

	for (n = 0; n < count; n++) {
		GUIEVT *evt = QUEUE_AT(n);

		CLEARS(&ev);
		ev.type  = (u8)evt->type;

		// EVM_HANDLE is what lets the source be one of this extension's own
		// handles rather than a gob - and it is what the GC follows, so an
		// event still queued keeps its window or widget alive.
		ev.model = EVM_HANDLE;
		ev.hob   = evt->source;

		// A move over a screen names it by key - see GUIEVT.screen. The
		// handle is made here, where allocating is allowed; it is the
		// same handle `screens` gives for that display.
		if (!ev.hob && evt->screen[0]) ev.hob = Screen_Handle(evt->screen);
		if (!ev.hob) continue;

		switch (evt->type) {
		case EVT_KEY:
		case EVT_KEY_UP:
		case EVT_NAMED_KEY:
		case EVT_NAMED_KEY_UP:
			// The codepoint, or the EVK_* number - the event's type tells
			// the core which, so `evt/key` reads a char! or a word.
			ev.flags = (1 << EVF_HAS_CODE) | Event_Modifier_Bits(evt->value);
			ev.data  = (u32)evt->x;
			break;

		case EVT_OPEN:
		case EVT_CLOSE:
			// A tree-view's branch: the node's number in `nodes`. A window's
			// `close` is positional, like the rest.
			if (ev.hob->sym != Handle_GuiWidget) {
				ev.flags = (1 << EVF_HAS_XY) | Event_Modifier_Bits(evt->value);
				ev.data  = (((u32)(evt->y & 0xffff)) << 16) | ((u32)evt->x & 0xffff);
				break;
			}
			ev.flags = (1 << EVF_HAS_CODE);
			ev.data  = (u32)evt->value;
			break;

		case EVT_SORT:
			// The 1-based number of the column whose header was clicked.
		case EVT_SCROLL_LINE:
			// The signed number of lines. There is no room for a position
			// as well - see the note above.
			ev.flags = (1 << EVF_HAS_CODE);
			ev.data  = (u32)evt->value;
			break;

		case EVT_MENU_SELECT:
		case EVT_THEME_CHANGE:  // `light` or `dark`, the same way
			// The item's WORD, as a canon symbol id. EVF_HAS_SYM is what
			// makes `evt/code` read it back as a word instead of a number,
			// which is what keeps a menu handler a plain `switch`.
			ev.flags = (1 << EVF_HAS_SYM) | (1 << EVF_HAS_CODE);
			ev.data  = (u32)evt->value;
			break;

		case EVT_DROP_FILE:
		case EVT_DROP_TEXT: {
			// The source is a DROP handle rather than the target: it is what
			// carries the content, and it is made here because building it
			// allocates - see the note above Make_Drop_Handle.
			REBHOB *drop = Make_Drop_Handle(evt->source, evt->drop);
			if (drop) ev.hob = drop;
			ev.flags = (1 << EVF_HAS_XY);
			ev.data  = (((u32)(evt->y & 0xffff)) << 16) | ((u32)evt->x & 0xffff);
			Gui_Drop_Free(evt->drop);
			evt->drop = NULL;
			break;
		}

		default:
			// Everything else is positional: the pointer, or the new client
			// size for `resize`.
			ev.flags = (1 << EVF_HAS_XY) | Event_Modifier_Bits(evt->value);
			ev.data  = (((u32)(evt->y & 0xffff)) << 16) | ((u32)evt->x & 0xffff);
			break;
		}

		// An event fits whole into the value slot, so it crosses by value -
		// there is no series behind it. Copied rather than assigned through
		// the union member so that this does not depend on its spelling.
		CLEARS(&val);
		COPY_MEM(&val, &ev, sizeof(ev));
		RL_SET_VALUE(blk, at++, val, RXT_EVENT);
	}

	RL_PROTECT_GC(blk, 0);
	Event_Tail += count;

	// The queue is empty again, so the next event may ring the doorbell.
	Event_Rung = FALSE;

	RXA_SERIES(frm, 1) = blk;
	RXA_INDEX(frm, 1) = 0;
	RXA_TYPE(frm, 1) = RXT_BLOCK;
	return RXR_VALUE;
}


/***********************************************************************
**  add-button / add-check / add-radio
**      window [handle!] text [string!] offset [pair!] size [pair!]
**      add-radio also takes /group id [integer!]
**
**  The widget handle is independent of the window handle: it can be kept,
**  dropped or released on its own, and it survives the window's death as
**  a closed widget rather than as a dangling pointer.
***********************************************************************/
static int Add_Button_Control(RXIFRM *frm, REBCNT kind)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBYTE    *text = NULL;
	REBCNT     text_len = 0;
	REBINT     x, y, w, h, req_w, req_h;
	int        len;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	len = RL_GET_UTF8_STRING(RXA_SERIES(frm, 2), RXA_INDEX(frm, 2), (void**)&text);
	if (len > 0) text_len = (REBCNT)len;

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w < 0 || h < 0) RETURN_ERROR(ERR_BAD_SIZE);

	// A zero axis asks for the natural size. It cannot be measured before
	// the control exists and has its font, so the control is created at a
	// placeholder and Attach_Widget() resizes it - which is also why the
	// REQUESTED size is what gets passed there.
	req_w = w; req_h = h;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid); // every field defined, whatever the pool handed back
	wid->hob   = hob;
	wid->kind  = kind;
	wid->owner  = win;
	wid->parent = panel; // read by the backend to pick the native parent

	// Only add-radio has this refinement, so only a radio may read it.
	if (kind == W_GUI_WIDGET_RADIO && RXA_REF(frm, 5))
		wid->group = (REBCNT)RXA_INT32(frm, 6);

	if (!Gui_Create_Button_Control(wid, win, x, y, w, h, text, text_len)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	// Linked here rather than by the backend, so that the list has exactly
	// one owner and both platforms behave the same.
	Attach_Widget(wid, win, req_w, req_h);

	RETURN_HANDLE(hob);
}

COMMAND cmd_gui_add_button(RXIFRM *frm, void *ctx)
{
	return Add_Button_Control(frm, W_GUI_WIDGET_BUTTON);
}

COMMAND cmd_gui_add_check(RXIFRM *frm, void *ctx)
{
	return Add_Button_Control(frm, W_GUI_WIDGET_CHECK);
}

// A push button which stays pushed: on or off like a check, looking like a
// button. Everything a check does - `state`, `click` after the state has
// settled - it does the same way.
COMMAND cmd_gui_add_toggle(RXIFRM *frm, void *ctx)
{
	return Add_Button_Control(frm, W_GUI_WIDGET_TOGGLE);
}

COMMAND cmd_gui_add_radio(RXIFRM *frm, void *ctx)
{
	return Add_Button_Control(frm, W_GUI_WIDGET_RADIO);
}


/***********************************************************************
**  add-image window [handle!] image [image!] offset [pair!] /size sz
**
**  The widget keeps a reference to the image, not a copy of it: drawing
**  into that same image and calling `redraw` is all it takes to change
**  what is on screen. Without /size the widget takes the image's own
**  size; with it, the image is scaled into the box.
***********************************************************************/
COMMAND cmd_gui_add_image(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBSER    *img = (REBSER*)RXA_IMAGE(frm, 2);
	REBINT     x, y, w, h;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
	if (!img) RETURN_ERROR(ERR_BAD_IMAGE);

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;

	if (RXA_REF(frm, 4)) { // /size
		w = (REBINT)RXA_PAIR(frm, 5).x;
		h = (REBINT)RXA_PAIR(frm, 5).y;
	} else {
		w = (REBINT)RXA_IMAGE_WIDTH(frm, 2);
		h = (REBINT)RXA_IMAGE_HEIGHT(frm, 2);
	}
	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid); // every field defined, whatever the pool handed back
	wid->hob   = hob;
	wid->kind  = W_GUI_WIDGET_IMAGE;
	wid->owner  = win;
	wid->parent = panel; // read by the backend to pick the native parent

	// The GC marks a handle context's series - which is exactly what keeps
	// the image alive for as long as a widget is showing it. The argument
	// is stored as it arrived, dimensions and all, rather than rebuilt
	// from the series: the index field an image! shares with them is the
	// trap this extension has been caught by before.
	if (!Hob_Set_Payload(hob, &RXA_ARG(frm, 2), RXT_IMAGE)) {
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_HANDLE);
	}

	if (!Gui_Create_Image(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		hob->series = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	Attach_Widget(wid, win, w, h);

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  add-panel parent [handle!] offset [pair!] size [pair!]
**            /border /title text [string!]
**
**  A panel is a widget like any other - it just happens to be something
**  other widgets can name as their parent.
**
**  /border draws a frame around it and /title puts a caption in that
**  frame; a caption implies the frame, because a group box without one
**  is just floating text. Neither moves anything the panel holds: a
**  child is positioned from the panel's own top-left either way, so an
**  edge can be turned on later without relaying anything out.
***********************************************************************/
COMMAND cmd_gui_add_panel(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBYTE    *text = NULL;
	REBCNT     text_len = 0;
	REBINT     x, y, w, h;
	int        len;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 2).x;
	y = (REBINT)RXA_PAIR(frm, 2).y;
	w = (REBINT)RXA_PAIR(frm, 3).x;
	h = (REBINT)RXA_PAIR(frm, 3).y;
	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	if (RXA_REF(frm, 5)) { // /title
		len = RL_GET_UTF8_STRING(RXA_SERIES(frm, 6), RXA_INDEX(frm, 6),
		                         (void**)&text);
		if (len > 0) text_len = (REBCNT)len;
	}

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid); // every field defined, whatever the pool handed back
	wid->hob    = hob;
	wid->kind   = W_GUI_WIDGET_PANEL;
	wid->owner  = win;
	wid->parent = panel; // panels nest like anything else

	// /title implies /border - the caption is drawn INTO the frame
	if (RXA_REF(frm, 4) || RXA_REF(frm, 5)) wid->state = GUI_PANEL_BORDER;

	if (!Gui_Create_Panel(wid, win, x, y, w, h, text, text_len)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	Attach_Widget(wid, win, w, h);

	RETURN_HANDLE(hob);
}


COMMAND cmd_gui_remove_widget(RXIFRM *frm, void *ctx)
{
	GUIWIDGET *wid = Frm_Widget(frm, 1);

	if (!wid) RETURN_ERROR(ERR_INVALID_HANDLE);

	if (wid->handle) {
		// A panel takes its contents with it, so their handles are told
		// before the native control - and everything under it - is gone.
		if (Kind_Is_Container(wid->kind)) Close_Contents_Of(wid);
		Gui_Destroy_Widget(wid); // the native control
		Gui_Widget_Closed(wid);  // the Rebol side of it
	}
	return RXR_TRUE;
}


/***********************************************************************
**  The label and the two text entries only differ in which kind they
**  ask for, so the three commands below are one function with three
**  entry points - the arguments and the failure paths are identical.
**
**      add-<kind> window [handle!] text [string!] offset size [pair!]
***********************************************************************/
static int Add_Text_Control(RXIFRM *frm, REBCNT kind)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBYTE    *text = NULL;
	REBCNT     text_len = 0;
	REBINT     x, y, w, h, req_w, req_h;
	int        len;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	len = RL_GET_UTF8_STRING(RXA_SERIES(frm, 2), RXA_INDEX(frm, 2), (void**)&text);
	if (len > 0) text_len = (REBCNT)len;

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w < 0 || h < 0) RETURN_ERROR(ERR_BAD_SIZE);

	// A zero axis asks for the natural size - see Add_Button_Control.
	req_w = w; req_h = h;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid); // every field defined, whatever the pool handed back
	wid->hob   = hob;
	wid->kind  = kind; // read by the backend to pick the native control
	wid->owner  = win;
	wid->parent = panel; // read by the backend to pick the native parent
	// `/secure` is decided before the control exists: on macOS a masked
	// field is a different cell, not a property of an ordinary one.
	if (kind == W_GUI_WIDGET_FIELD && RXA_REF(frm, 6)) wid->state |= GUI_TEXT_SECURE;

	if (!Gui_Create_Text_Control(wid, win, x, y, w, h, text, text_len)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}
	// `/flat` - before Attach_Widget, so a natural size leaves no room for
	// a border that is not there. A label has no such refinement.
	if (Kind_Has_Border(kind) && RXA_REF(frm, 5)) Gui_Widget_Set_Border(wid, FALSE);

	Attach_Widget(wid, win, req_w, req_h);

	RETURN_HANDLE(hob);
}

/***********************************************************************
**  add-slider / add-progress
**      window [handle!] offset [pair!] size [pair!] /value val
**
**  Neither carries a label, so the arguments are one short of the rest.
***********************************************************************/
static int Add_Range_Control(RXIFRM *frm, REBCNT kind)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBINT     x, y, w, h;
	REBDEC     value = 0.0;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 2).x;
	y = (REBINT)RXA_PAIR(frm, 2).y;
	w = (REBINT)RXA_PAIR(frm, 3).x;
	h = (REBINT)RXA_PAIR(frm, 3).y;
	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	if (RXA_REF(frm, 4)) { // /value
		value = RXA_DEC64(frm, 5);
		if (value < 0.0) value = 0.0;
		if (value > 1.0) value = 1.0;
	}

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid); // every field defined, whatever the pool handed back
	wid->hob   = hob;
	wid->kind  = kind;
	wid->owner  = win;
	wid->parent = panel; // read by the backend to pick the native parent

	if (!Gui_Create_Range_Control(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}
	Gui_Widget_Set_Value(wid, value);

	Attach_Widget(wid, win, w, h);

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  add-drop-list / add-drop-down / add-text-list
**      window items [block!] offset [pair!] size [pair!] /index n [integer!]
**
**  Three widgets, one body: a drop-list picks one of a list behind a
**  button; a drop-down is the same box with an editable text field
**  added, so the user can also type a value that is not in the list;
**  a text-list shows the same strings in a fixed box instead of behind
**  a button. `wid->kind` is what a backend tells them apart by.
***********************************************************************/
static int Add_List_Control(RXIFRM *frm, REBCNT kind)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBINT     x, y, w, h, req_w, req_h;
	REBOOL     created;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w < 0 || h < 0) RETURN_ERROR(ERR_BAD_SIZE);

	// A zero axis asks for the natural size - see Add_Button_Control.
	req_w = w; req_h = h;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid); // every field defined, whatever the pool handed back
	wid->hob   = hob;
	wid->kind  = kind;
	wid->owner  = win;
	wid->parent = panel; // read by the backend to pick the native parent

	switch (kind) {
	case W_GUI_WIDGET_TEXT_LIST: created = Gui_Create_Text_List(wid, win, x, y, w, h); break;
	case W_GUI_WIDGET_DROP_DOWN: created = Gui_Create_Combo_Box(wid, win, x, y, w, h); break;
	default:                     created = Gui_Create_Drop_List(wid, win, x, y, w, h); break;
	}
	if (!created) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	// `/flat` is the text-list's alone; a drop-list or a drop-down has no
	// refinement 7.
	if (kind == W_GUI_WIDGET_TEXT_LIST && RXA_REF(frm, 7)) Gui_Widget_Set_Border(wid, FALSE);

	Block_To_Items(wid, RXA_SERIES(frm, 2));
	// Nothing is picked unless asked for - a list which starts blank is a
	// normal thing to want.
	Gui_Widget_Set_Index(wid, RXA_REF(frm, 5) ? (REBINT)RXA_INT32(frm, 6) - 1 : -1);

	Attach_Widget(wid, win, req_w, req_h);

	RETURN_HANDLE(hob);
}


COMMAND cmd_gui_add_drop_list(RXIFRM *frm, void *ctx)
{
	return Add_List_Control(frm, W_GUI_WIDGET_DROP_LIST);
}

COMMAND cmd_gui_add_text_list(RXIFRM *frm, void *ctx)
{
	return Add_List_Control(frm, W_GUI_WIDGET_TEXT_LIST);
}


/***********************************************************************
**  add-list-view
**      parent columns [block!] offset [pair!] size [pair!]
**      /with cells [block!] /index n [integer!] /flat
**
**  Frame: 1 parent, 2 columns, 3 offset, 4 size, 5 /with, 6 cells,
**  7 /index, 8 n, 9 /flat.
***********************************************************************/
COMMAND cmd_gui_add_list_view(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBINT     x, y, w, h, req_w, req_h;
	REBCNT     count = 0;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
	if (!List_Columns_Valid(RXA_SERIES(frm, 2), RXA_INDEX(frm, 2), &count))
		RETURN_ERROR(ERR_BAD_COLUMNS);
	// The cells are checked here too, before anything is made - the same
	// test List_Set_Items applies.
	if (RXA_REF(frm, 5)) {
		REBCNT tail  = (REBCNT)RL_SERIES(RXA_SERIES(frm, 6), RXI_SER_TAIL);
		REBCNT index = RXA_INDEX(frm, 6);
		if (tail < index) tail = index;
		if (count == 0 ? (tail > index) : ((tail - index) % count != 0))
			RETURN_ERROR(ERR_BAD_CELLS);
	}

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w < 0 || h < 0) RETURN_ERROR(ERR_BAD_SIZE);

	// A zero axis asks for the natural size - see Add_Button_Control.
	req_w = w; req_h = h;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid);
	wid->hob    = hob;
	wid->kind   = W_GUI_WIDGET_LIST_VIEW;
	wid->owner  = win;
	wid->parent = panel;
	wid->picked = -1;

	// Before the control exists: it may ask for cells as soon as it does.
	if (!Hob_Scratch(hob, TRUE) || !Gui_Create_List_View(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	if (RXA_REF(frm, 9)) Gui_Widget_Set_Border(wid, FALSE);

	List_Set_Columns(wid, RXA_SERIES(frm, 2), RXA_INDEX(frm, 2));
	if (RXA_REF(frm, 5)) List_Set_Items(wid, RXA_SERIES(frm, 6), RXA_INDEX(frm, 6));
	List_Set_Index(wid, RXA_REF(frm, 7) ? (REBINT)RXA_INT32(frm, 8) - 1 : -1);

	Attach_Widget(wid, win, req_w, req_h);

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  add-tree-view parent items [block!] offset [pair!] size [pair!] /flat
**
**  Frame: 1 parent, 2 items, 3 offset, 4 size, 5 /flat.
***********************************************************************/
COMMAND cmd_gui_add_tree_view(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBINT     x, y, w, h, req_w, req_h;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w < 0 || h < 0) RETURN_ERROR(ERR_BAD_SIZE);

	// A zero axis asks for the natural size - see Add_Button_Control.
	req_w = w; req_h = h;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid);
	wid->hob    = hob;
	wid->kind   = W_GUI_WIDGET_TREE_VIEW;
	wid->owner  = win;
	wid->parent = panel;
	wid->picked = -1;

	if (!Gui_Create_Tree_View(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	if (RXA_REF(frm, 5)) Gui_Widget_Set_Border(wid, FALSE);
	Tree_Set_Items(wid, RXA_SERIES(frm, 2), RXA_INDEX(frm, 2));

	Attach_Widget(wid, win, req_w, req_h);

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  add-date-field parent offset size /date when [date!] /time
***********************************************************************/
COMMAND cmd_gui_add_date_field(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBINT     x, y, w, h, req_w, req_h;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 2).x;
	y = (REBINT)RXA_PAIR(frm, 2).y;
	w = (REBINT)RXA_PAIR(frm, 3).x;
	h = (REBINT)RXA_PAIR(frm, 3).y;
	if (w < 0 || h < 0) RETURN_ERROR(ERR_BAD_SIZE);

	// A zero axis asks for the natural size - see Add_Button_Control.
	req_w = w; req_h = h;
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid);
	wid->hob    = hob;
	wid->kind   = W_GUI_WIDGET_DATE_FIELD;
	wid->owner  = win;
	wid->parent = panel;
	// Decided before the control exists - which parts it shows is a
	// creation style on Windows and a creation-time choice on macOS.
	if (RXA_REF(frm, 6)) wid->state |= GUI_DATE_TIME;

	if (!Gui_Create_Date_Field(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	// Both controls start at the current date and time; `/date` replaces
	// them - the date, and the time of day too when it has one.
	if (RXA_REF(frm, 4)) {
		GUIDATE d;
		if (Date_From_Arg(wid, (u32)RXA_DATE(frm, 5), RXA_DATE_TIME(frm, 5), &d))
			Gui_Widget_Set_Date(wid, &d);
	}

	Attach_Widget(wid, win, req_w, req_h);

	RETURN_HANDLE(hob);
}


COMMAND cmd_gui_add_slider(RXIFRM *frm, void *ctx)
{
	return Add_Range_Control(frm, W_GUI_WIDGET_SLIDER);
}

COMMAND cmd_gui_add_progress(RXIFRM *frm, void *ctx)
{
	return Add_Range_Control(frm, W_GUI_WIDGET_PROGRESS);
}


COMMAND cmd_gui_add_text(RXIFRM *frm, void *ctx)
{
	return Add_Text_Control(frm, W_GUI_WIDGET_TEXT);
}

COMMAND cmd_gui_add_field(RXIFRM *frm, void *ctx)
{
	return Add_Text_Control(frm, W_GUI_WIDGET_FIELD);
}

COMMAND cmd_gui_add_area(RXIFRM *frm, void *ctx)
{
	return Add_Text_Control(frm, W_GUI_WIDGET_AREA);
}


/***********************************************************************
**  redraw target [handle!]
**
**  Takes either kind of handle, because "I changed the pixels, show them"
**  is the same request whether it is aimed at one widget or a window.
***********************************************************************/
COMMAND cmd_gui_redraw(RXIFRM *frm, void *ctx)
{
	GUIWIDGET *wid;
	GUIWIN    *win;

	if ((wid = Frm_Widget(frm, 1)) != NULL) {
		Gui_Widget_Redraw(wid);
		return RXR_TRUE;
	}
	if ((win = Frm_Window(frm, 1)) != NULL) {
		Gui_Window_Redraw(win);
		return RXR_TRUE;
	}
	RETURN_ERROR(ERR_INVALID_HANDLE);
}


/***********************************************************************
**  add-drop-down window items [block!] offset [pair!] size [pair!]
**                /index n [integer!]
**
**  The editable combo box: everything a drop-list is, plus a text field
**  the user can type into - see the note above Add_List_Control.
***********************************************************************/
COMMAND cmd_gui_add_drop_down(RXIFRM *frm, void *ctx)
{
	return Add_List_Control(frm, W_GUI_WIDGET_DROP_DOWN);
}


/***********************************************************************
**  add-line parent offset size
**
**  A separator. Which way it runs follows the box, as for a slider:
**  wider than tall is horizontal. A zero axis is not measured - a line
**  has no content - but means the line's own thickness, so `200x0` is
**  a horizontal rule 200 long.
***********************************************************************/
#define GUI_LINE_THICKNESS 2

COMMAND cmd_gui_add_line(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBINT     x, y, w, h;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 2).x;
	y = (REBINT)RXA_PAIR(frm, 2).y;
	w = (REBINT)RXA_PAIR(frm, 3).x;
	h = (REBINT)RXA_PAIR(frm, 3).y;
	if (w < 0 || h < 0 || (w == 0 && h == 0)) RETURN_ERROR(ERR_BAD_SIZE);
	if (w == 0) w = GUI_LINE_THICKNESS;
	if (h == 0) h = GUI_LINE_THICKNESS;

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid);
	wid->hob    = hob;
	wid->kind   = W_GUI_WIDGET_LINE;
	wid->owner  = win;
	wid->parent = panel;

	if (!Gui_Create_Line(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	Attach_Widget(wid, win, w, h);

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  add-tab-panel parent labels [block!] offset size /index n
**
**  The frame first, then a tab and a page for each label, in order.
**  The pages are PANEL widgets like any other - they go on the window's
**  list and into the tab-panel's `children`, and a script puts widgets
**  on them as on a panel - with `group` saying which tab each is.
**
**  Anything in the block which is not a string is skipped, as for a
**  drop-list; a block with no string at all is refused, since a
**  tab-panel with nothing to show is not a useful thing to make.
***********************************************************************/
COMMAND cmd_gui_add_tab_panel(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIDGET *panel = NULL;
	GUIWIN    *win = Frm_Parent(frm, 1, &panel);
	REBSER    *labels = RXA_SERIES(frm, 2);
	REBINT     x, y, w, h;
	REBCNT     n, t, pages = 0;
	RXIARG     val;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	CLEARS(wid);
	wid->hob    = hob;
	wid->kind   = W_GUI_WIDGET_TAB_PANEL;
	wid->owner  = win;
	wid->parent = panel;

	if (!Gui_Create_Tab_Panel(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		wid->parent = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}
	// Attached before the pages, so that they land in ITS children.
	Attach_Widget(wid, win, w, h);

	for (n = RXA_INDEX(frm, 2); (t = RL_GET_VALUE(labels, n, &val)) != 0; n++) {
		REBYTE    *utf8 = NULL;
		int        len;
		REBHOB    *page_hob;
		GUIWIDGET *page;

		if (t == RXT_END) break;
		if (t != RXT_STRING) continue;
		len = RL_GET_UTF8_STRING((REBSER*)val.series, val.index, (void**)&utf8);
		if (len < 0 || !Gui_Widget_Add_Item(wid, utf8, (REBCNT)len)) continue;

		page_hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
		if (!page_hob) break;
		page = (GUIWIDGET*)page_hob->data;
		CLEARS(page);
		page->hob    = page_hob;
		page->kind   = W_GUI_WIDGET_PANEL;
		page->owner  = win;
		page->parent = wid;
		page->group  = ++pages;   // its tab, 1-based

		if (!Gui_Create_Tab_Page(page, wid, win)) {
			page->owner  = NULL;
			page->parent = NULL;
			RL_FREE_HANDLE_CONTEXT(page_hob);
			break;
		}
		// Any non-zero size: a page's box is the backend's, never fitted.
		Attach_Widget(page, win, 1, 1);
	}

	if (pages == 0) {
		Gui_Destroy_Widget(wid);
		Gui_Widget_Closed(wid);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	Gui_Widget_Set_Index(wid, RXA_REF(frm, 5) ? (REBINT)RXA_INT32(frm, 6) - 1 : 0);

	RETURN_HANDLE(hob);
}


/***********************************************************************
**  within? point offset size
**
**  Whether a point is in a box: the left and top edges in, the right and
**  bottom out, so boxes laid edge to edge never both claim a point. The
**  question every mouse handler asks - `within? evt/offset w/at w/size`.
**  A pair! holds floats, so the compare is on those as they are.
***********************************************************************/
COMMAND cmd_gui_withinq(RXIFRM *frm, void *ctx)
{
	REBXYF p = RXA_PAIR(frm, 1);
	REBXYF o = RXA_PAIR(frm, 2);
	REBXYF s = RXA_PAIR(frm, 3);

	return (p.x >= o.x && p.y >= o.y && p.x < o.x + s.x && p.y < o.y + s.y)
		? RXR_TRUE : RXR_FALSE;
}


/***********************************************************************
**  popup-menu target items /at offset
**
**  The dialect is walked by Block_To_Menu, which numbers the items in
**  the window's id table - the one the menu bar uses. So that table is
**  set aside for the popup's own and put back afterwards: the bar keeps
**  its ids, and the popup's are gone the moment it closes.
**
**  The native menu runs a loop of its own until it closes. Events that
**  arrive meanwhile are queued as usual - the queue never allocates -
**  and come out of the next `poll-events`.
***********************************************************************/
COMMAND cmd_gui_popup_menu(RXIFRM *frm, void *ctx)
{
	GUIWIN    *win = Frm_Window(frm, 1);
	GUIWIDGET *wid;
	void      *root, *bar;
	REBCNT    *ids;
	REBYTE    *on;
	REBCNT     count, id, word = 0;
	REBOOL     at = RXA_REF(frm, 3) ? TRUE : FALSE;
	REBINT     x = 0, y = 0;

	if (!win && (wid = Frm_Widget(frm, 1)) != NULL && wid->handle) win = wid->owner;
	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);
	if (at) {
		x = (REBINT)RXA_PAIR(frm, 4).x;
		y = (REBINT)RXA_PAIR(frm, 4).y;
	}

	root = Gui_Popup_Begin(win);
	if (!root) RETURN_ERROR(ERR_NO_WIDGET);

	// The bar's table aside ...
	bar = win->menu; ids = win->menu_ids; on = win->menu_on; count = win->menu_count;
	win->menu = root; win->menu_ids = NULL; win->menu_on = NULL; win->menu_count = 0;

	Block_To_Menu(win, RXA_SERIES(frm, 2), RXA_INDEX(frm, 2), root);
	id = Gui_Popup_Track(win, root, at, x, y);
	if (id > 0 && id <= win->menu_count) word = win->menu_ids[id - 1];

	// ... and back.
	Menu_Free_Ids(win);
	win->menu = bar; win->menu_ids = ids; win->menu_on = on; win->menu_count = count;
	Gui_Popup_Free(win, root);

	if (!word) return RXR_NONE;
	RXA_WORD(frm, 1) = (i32)word;
	RXA_TYPE(frm, 1) = RXT_WORD;
	return RXR_VALUE;
}


//== handle callbacks =========================================================

int GuiWindow_free(void *hndl)
{
	REBHOB *hob;
	GUIWIN *win;

	if (!hndl) return 0;
	hob = (REBHOB*)hndl;
	win = (GUIWIN*)hob->data;
	if (!win) return 0;

	// Reached through an explicit `release`, or at shutdown. A window still
	// open at this point has to go; Gui_Window_Closed() then drops whatever
	// it had queued. This cannot be refused, so any dialog open on it goes
	// first, newest first.
	if (win->handle) {
		REBCNT n = Modal_Depth;
		while (n-- > 0) {
			GUIWIN *dlg = Modal_Stack[n];
			GUIWIN *owner;
			if (n >= Modal_Depth) continue;   // the stack shrank meanwhile
			for (owner = (GUIWIN*)dlg->modal_owner; owner; owner = (GUIWIN*)owner->modal_owner) {
				if (owner != win) continue;
				Modal_Remove(dlg);
				if (dlg->handle) Gui_Close_Window(dlg);
				break;
			}
		}
		Modal_Remove(win);
		Gui_Close_Window(win);
	}

	debug_print("releasing GUI window handle: %p\n", (void*)win);
	// The default font's family name and the menu's word table are plain
	// malloc'd memory owned by the window - the GC knows nothing about
	// either, so this is where they go. The menu BLOCK is in hob->series
	// and needs nothing: dropping the reference is enough.
	Font_Free(&win->font);
	Gui_Menu_Free(win);
	Menu_Free_Ids(win);
	hob->series = NULL;
	CLEARS(win);
	UNMARK_HOB(hob);
	return 0;
}


int GuiWindow_get_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg)
{
	GUIWIN *win = (GUIWIN*)hob->data;
	REBINT a, b;

	word = RL_FIND_WORD(Gui_arg_words, word);

	// A closed window still answers - with none, rather than an error.
	//
	// `word != 0` first: RL_FIND_WORD answers 0 for a word this extension
	// does not know, and PD_Handle only supplies `type` for a path this
	// callback REFUSED. Answering none here would take `closed/type` with
	// it, and a handle's type is true whether or not the window is gone.
	if (word != 0 && !win->handle
	    && word != W_GUI_ARG_OPENQ && word != W_GUI_ARG_ID) {
		*type = RXT_NONE;
		return PE_USE;
	}

	switch (word) {
	case W_GUI_ARG_TITLE: {
		REBSER *str = Gui_Get_Title(win);
		if (!str) { *type = RXT_NONE; break; }
		arg->series = str;
		arg->index  = 0;
		*type = RXT_STRING;
		break; }

	case W_GUI_ARG_SIZE:
		if (!Gui_Get_Size(win, &a, &b)) { *type = RXT_NONE; break; }
		arg->pair.x = (float)a;
		arg->pair.y = (float)b;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_OFFSET:
		if (!Gui_Get_Offset(win, &a, &b)) { *type = RXT_NONE; break; }
		arg->pair.x = (float)a;
		arg->pair.y = (float)b;
		*type = RXT_PAIR;
		break;

	// Mouse offsets are measured from the client area's top-left corner,
	// so a window's own `at` is that corner. It is here only so that
	// `evt/offset - evt/source/at` needs no "is it a window?" test.
	case W_GUI_ARG_AT:
		arg->pair.x = 0;
		arg->pair.y = 0;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_ID:
		*type = RXT_INTEGER;
		arg->int64 = (i64)(REBUPT)win->handle;
		break;

	case W_GUI_ARG_DARK_CONTROLSQ:
		*type = RXT_LOGIC;
		arg->int32a = (win->flags & GUIW_DARK_CONTROLS) ? 1 : 0;
		break;

	case W_GUI_ARG_MODALQ:
		*type = RXT_LOGIC;
		arg->int32a = (win->flags & GUIW_MODAL) ? 1 : 0;
		break;

	case W_GUI_ARG_KEYSQ:
		*type = RXT_LOGIC;
		arg->int32a = (win->flags & GUIW_KEYS) ? 1 : 0;
		break;

	// Asked of the platform each time rather than taken from GUIW_DARK,
	// which is only what was last REPORTED.
	case W_GUI_ARG_DARKQ:
		*type = RXT_LOGIC;
		arg->int32a = Gui_Window_Dark(win) ? 1 : 0;
		break;

	// The same handle `screens` hands out for that display, so
	// `win/screen == first screens` asks the obvious question.
	case W_GUI_ARG_SCREEN: {
		REBYTE  key[GUI_SCREEN_KEY];
		REBHOB *scr;
		if (!Gui_Window_Screen(win, key) || !(scr = Screen_Handle(key))) {
			*type = RXT_NONE;
			break;
		}
		Set_Handle_Arg(arg, scr);
		*type = RXT_HANDLE;
		break; }

	case W_GUI_ARG_OPENQ:
		*type = RXT_LOGIC;
		arg->int32a = (win->handle != NULL);
		break;

	// Sizes are logical units everywhere, so this is only needed to make
	// an IMAGE land on device pixels one for one - see the README.
	case W_GUI_ARG_SCALE:
		*type = RXT_DECIMAL;
		arg->dec64 = (double)Gui_Get_Scale(win);
		break;

	case W_GUI_ARG_RESIZABLEQ:
		*type = RXT_LOGIC;
		arg->int32a = Gui_Get_Resizable(win) ? 1 : 0;
		break;

	case W_GUI_ARG_TITLEQ:
		*type = RXT_LOGIC;
		arg->int32a = Gui_Get_Title_Bar(win) ? 1 : 0;
		break;

	// A titled window always has its frame, whatever is remembered for
	// when it has none.
	case W_GUI_ARG_BORDERQ:
		*type = RXT_LOGIC;
		arg->int32a = (Gui_Get_Title_Bar(win) || (win->flags & GUIW_BORDER)) ? 1 : 0;
		break;

	// The client area's own colour, or none when the system's is used.
	// A see-through window has no colour to report either.
	case W_GUI_ARG_BACKGROUND:
		if (!GUI_COLOR_HAS(win->background)) { *type = RXT_NONE; break; }
		CLEARS(arg);
		arg->tuple_len      = 3;
		arg->tuple_bytes[0] = (REBYTE)GUI_COLOR_R(win->background);
		arg->tuple_bytes[1] = (REBYTE)GUI_COLOR_G(win->background);
		arg->tuple_bytes[2] = (REBYTE)GUI_COLOR_B(win->background);
		*type = RXT_TUPLE;
		break;

	case W_GUI_ARG_TRANSPARENTQ:
		*type = RXT_LOGIC;
		arg->int32a = GUI_BG_IS_CLEAR(win->background) ? 1 : 0;
		break;

	case W_GUI_ARG_DROPQ:
		*type = RXT_LOGIC;
		arg->int32a = (win->flags & GUIW_ACCEPTS_DROP) ? 1 : 0;
		break;

	/*******************************************************************
	**  What the NEXT widget will be created with - not a description of
	**  anything currently on screen. Unlike a widget's font, which is
	**  read back from the control, this is the extension's own note to
	**  itself, so it reports exactly what was set.
	*******************************************************************/
	case W_GUI_ARG_FONT:
		if (!win->font.name) { *type = RXT_NONE; break; }
		arg->series = RL_DECODE_UTF_STRING((REBYTE*)win->font.name,
			(REBCNT)strlen(win->font.name), 8, FALSE, FALSE);
		if (!arg->series) { *type = RXT_NONE; break; }
		arg->index = 0;
		*type = RXT_STRING;
		break;

	case W_GUI_ARG_FONT_SIZE:
		if (win->font.size <= 0) { *type = RXT_NONE; break; }
		*type = RXT_INTEGER;
		arg->int64 = (i64)win->font.size;
		break;

	case W_GUI_ARG_BOLDQ:
		*type = RXT_LOGIC;
		arg->int32a = ((win->font.style & GUI_FONT_BOLD) != 0);
		break;

	case W_GUI_ARG_ITALICQ:
		*type = RXT_LOGIC;
		arg->int32a = ((win->font.style & GUI_FONT_ITALIC) != 0);
		break;

	// The very block which was assigned, kept alive in hob->series.
	// Every widget the WINDOW holds directly - the ones inside a panel or
	// an image widget belong to that container's own list.
	case W_GUI_ARG_CHILDREN: {
		REBSER *kids = Hob_Children(hob, FALSE);
		// An empty block rather than none, so that a caller can always
		// `foreach` the answer without asking whether there is one. Not
		// stored: a container nobody put anything in keeps no slot.
		if (!kids) kids = (REBSER*)RL_MAKE_BLOCK(0);
		if (!kids) { *type = RXT_NONE; break; }
		arg->series = kids;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	case W_GUI_ARG_MENU: {
		REBSER *menu = Hob_Payload(hob);
		if (!menu) { *type = RXT_NONE; break; }
		arg->series = menu;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	/*******************************************************************
	**  Every item's word and whether it is selectable, as pairs. It
	**  reports ALL of them, while setting merges - so reading is a
	**  picture of the whole bar and writing is a change to part of it.
	*******************************************************************/
	case W_GUI_ARG_MENU_ENABLEDQ: {
		REBSER *blk;
		REBCNT  n, out = 0;
		RXIARG  val;

		blk = (REBSER*)RL_MAKE_BLOCK(win->menu_count * 2);
		if (!blk) { *type = RXT_NONE; break; }
		RL_PROTECT_GC(blk, 1);

		for (n = 0; n < win->menu_count; n++) {
			if (win->menu_ids[n] == 0) continue; // a label with no id
			CLEARS(&val);
			val.int32a = (i32)win->menu_ids[n];
			RL_SET_VALUE(blk, out++, val, RXT_WORD);
			CLEARS(&val);
			val.int32a = win->menu_on[n] ? 1 : 0;
			RL_SET_VALUE(blk, out++, val, RXT_LOGIC);
		}

		RL_PROTECT_GC(blk, 0);
		arg->series = blk;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	default:
		return PE_BAD_SELECT;
	}
	return PE_USE;
}


int GuiWindow_set_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg)
{
	GUIWIN *win = (GUIWIN*)hob->data;

	if (!win->handle) return PE_BAD_SET; // closed windows are read-only

	switch (RL_FIND_WORD(Gui_arg_words, word)) {
	case W_GUI_ARG_DARK_CONTROLSQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) win->flags |=  GUIW_DARK_CONTROLS;
		else             win->flags &= ~(REBCNT)GUIW_DARK_CONTROLS;
		Gui_Window_Dark_Controls(win, arg->int32a ? TRUE : FALSE);
		break;

	// Only a flag: the backends look at it for every key, so there is
	// nothing to install or remove here.
	case W_GUI_ARG_KEYSQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) win->flags |=  GUIW_KEYS;
		else             win->flags &= ~(REBCNT)GUIW_KEYS;
		break;

	case W_GUI_ARG_TITLE: {
		REBYTE *utf8 = NULL;
		int len;
		if (*type != RXT_STRING) return PE_BAD_SET_TYPE;
		len = RL_GET_UTF8_STRING((REBSER*)arg->series, arg->index, (void**)&utf8);
		if (len < 0) return PE_BAD_SET;
		Gui_Set_Title(win, utf8, (REBCNT)len);
		break; }

	case W_GUI_ARG_SIZE:
		if (*type != RXT_PAIR) return PE_BAD_SET_TYPE;
		if (arg->pair.x <= 0 || arg->pair.y <= 0) return PE_BAD_RANGE;
		Gui_Set_Size(win, (REBINT)arg->pair.x, (REBINT)arg->pair.y);
		break;

	case W_GUI_ARG_OFFSET:
		if (*type != RXT_PAIR) return PE_BAD_SET_TYPE;
		Gui_Set_Offset(win, (REBINT)arg->pair.x, (REBINT)arg->pair.y);
		break;

	/*******************************************************************
	**  Setting a default touches nothing that already exists - it is
	**  read once, by Attach_Widget, when the next widget is made.
	*******************************************************************/
	case W_GUI_ARG_FONT: {
		REBYTE *utf8 = NULL;
		int len = 0;
		if (*type == RXT_STRING) {
			len = RL_GET_UTF8_STRING((REBSER*)arg->series, arg->index,
			                         (void**)&utf8);
			if (len < 0) return PE_BAD_SET;
		} else if (*type != RXT_NONE) {
			return PE_BAD_SET_TYPE;
		}
		if (!Font_Set_Name(&win->font, utf8, (REBCNT)len)) return PE_BAD_SET;
		break; }

	case W_GUI_ARG_FONT_SIZE:
		if (*type == RXT_NONE) {
			win->font.size = 0;
		} else if (*type == RXT_INTEGER) {
			if (arg->int64 <= 0 || arg->int64 > 1000) return PE_BAD_RANGE;
			win->font.size = (REBINT)arg->int64;
		} else {
			return PE_BAD_SET_TYPE;
		}
		break;

	case W_GUI_ARG_BOLDQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) win->font.style |=  GUI_FONT_BOLD;
		else             win->font.style &= ~(REBCNT)GUI_FONT_BOLD;
		break;

	case W_GUI_ARG_ITALICQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) win->font.style |=  GUI_FONT_ITALIC;
		else             win->font.style &= ~(REBCNT)GUI_FONT_ITALIC;
		break;

	case W_GUI_ARG_RESIZABLEQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		Gui_Set_Resizable(win, arg->int32a ? TRUE : FALSE);
		break;

	// Taking the border off takes the title bar with it, so the window
	// stops reporting `close` and can only be moved by `offset`.
	case W_GUI_ARG_TITLEQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		Gui_Set_Title_Bar(win, arg->int32a ? TRUE : FALSE);
		break;

	// Remembered either way; it only SHOWS while there is no title bar,
	// and the backend applies it again whenever `title?` goes false.
	case W_GUI_ARG_BORDERQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) win->flags |=  GUIW_BORDER;
		else             win->flags &= ~(REBCNT)GUIW_BORDER;
		Gui_Window_Apply_Border(win);
		break;

	// One field, three states, exactly as on a widget: giving a colour
	// turns see-through off, and `transparent?: false` goes back to the
	// system colour rather than to a colour set earlier.
	case W_GUI_ARG_BACKGROUND:
		if (*type == RXT_NONE) {
			win->background = 0;
		} else if (*type == RXT_TUPLE) {
			if (arg->tuple_len < 3) return PE_BAD_SET;
			win->background = GUI_COLOR_OF(arg->tuple_bytes[0],
			                               arg->tuple_bytes[1],
			                               arg->tuple_bytes[2]);
		} else {
			return PE_BAD_SET_TYPE;
		}
		Gui_Window_Set_Background(win);
		break;

	case W_GUI_ARG_TRANSPARENTQ:
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		win->background = arg->int32a ? GUI_BG_CLEAR : 0;
		Gui_Window_Set_Background(win);
		break;

	case W_GUI_ARG_DROPQ:
		// Off by default: a window which silently swallows a drop is worse
		// than one which visibly refuses it, so accepting is asked for.
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		Gui_Window_Set_Drop(win, arg->int32a ? TRUE : FALSE);
		break;

	case W_GUI_ARG_MENU:
		// A menu is replaced whole, never edited in place: the block is
		// the description, and rebuilding from it is what keeps the two
		// from disagreeing. `none` takes the bar off.
		if (*type == RXT_NONE) {
			if (!Set_Menu(win, NULL, 0)) return PE_BAD_SET;
		} else if (*type == RXT_BLOCK) {
			if (!Set_Menu(win, (REBSER*)arg->series, arg->index))
				return PE_BAD_SET;
		} else {
			return PE_BAD_SET_TYPE;
		}
		break;

	/*******************************************************************
	**  Greying items out, as word/logic pairs. Only the words listed
	**  change - anything left out keeps whatever it had, so this is a
	**  small adjustment rather than a redeclaration of the menu.
	*******************************************************************/
	case W_GUI_ARG_MENU_ENABLEDQ: {
		REBSER *blk;
		REBCNT  n, t;
		RXIARG  item;
		REBCNT  word = 0;

		if (*type != RXT_BLOCK) return PE_BAD_SET_TYPE;
		blk = (REBSER*)arg->series;

		for (n = arg->index; (t = RL_GET_VALUE(blk, n, &item)) != 0; n++) {
			if (t == RXT_END) break;
			if (t == RXT_WORD) { word = (REBCNT)item.int32a; continue; }
			if (!word) continue;
			if (t == RXT_LOGIC || t == RXT_NONE) {
				REBOOL on = (t == RXT_LOGIC && item.int32a) ? TRUE : FALSE;
				REBCNT i;
				// Every item with that word, so one word used twice in a
				// menu turns both on or both off.
				for (i = 0; i < win->menu_count; i++) {
					if (win->menu_ids[i] != word) continue;
					win->menu_on[i] = on ? 1 : 0;
					Gui_Menu_Enable(win, i + 1, on);
				}
			}
			word = 0;
		}
		break; }

	default:
		return PE_BAD_SET;
	}
	return PE_OK;
}


int GuiWindow_mold(REBHOB *hob, REBSER *str)
{
	int len;
	GUIWIN *win;
	REBINT w = 0, h = 0;

	if (!str || !hob || !(win = (GUIWIN*)hob->data)) return 0;

	SERIES_TAIL(str) = 0;
	if (win->handle && Gui_Get_Size(win, &w, &h)) {
		APPEND_STRING(str, "0#%lx %dx%d", (unsigned long)(REBUPT)win->handle, w, h);
	} else {
		APPEND_STRING(str, "%s", "closed");
	}
	return len;
}


//== widget handle callbacks ==================================================

int GuiWidget_free(void *hndl)
{
	REBHOB    *hob;
	GUIWIDGET *wid;

	if (!hndl) return 0;
	hob = (REBHOB*)hndl;
	wid = (GUIWIDGET*)hob->data;
	if (!wid) return 0;

	if (wid->handle) {
		// `release` on a panel means the same as remove-widget on it.
		if (Kind_Is_Container(wid->kind)) Close_Contents_Of(wid);
		Gui_Destroy_Widget(wid);
		Gui_Widget_Closed(wid);
	}
	debug_print("releasing GUI widget handle: %p\n", (void*)wid);
	Tree_Free(wid);
	CLEARS(wid);
	UNMARK_HOB(hob);
	return 0;
}


int GuiWidget_get_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg)
{
	GUIWIDGET *wid = (GUIWIDGET*)hob->data;
	REBINT x, y, w, h;

	word = RL_FIND_WORD(Gui_arg_words, word);

	// A widget whose window has gone answers with none, like a closed
	// window does - except for `id`, which reports the null it now holds,
	// and for a word this extension does not know at all (word 0), which
	// has to be refused so that PD_Handle can still answer `type`.
	if (word != 0 && !wid->handle && word != W_GUI_ARG_ID) {
		*type = RXT_NONE;
		return PE_USE;
	}

	switch (word) {
	// Accessors which only make sense for one kind answer with none on the
	// others, rather than erroring - the same as a closed widget does.
	case W_GUI_ARG_TEXT: {
		REBSER *str;
		if (!Kind_Has_Text(wid->kind)) { *type = RXT_NONE; break; }
		if (wid->kind == W_GUI_WIDGET_TREE_VIEW) {
			// The label of the picked node - none when nothing is.
			GUITREE *tree = GUI_TREE_OF(wid);
			REBINT   n = Gui_Tree_Selected(wid);
			if (!tree || n < 0 || n >= (REBINT)tree->count) { *type = RXT_NONE; break; }
			str = RL_DECODE_UTF_STRING(tree->nodes[n].label, tree->nodes[n].len, 8, FALSE, FALSE);
			if (!str) { *type = RXT_NONE; break; }
			arg->series = str;
			arg->index  = 0;
			*type = RXT_STRING;
			break;
		}
		if (wid->kind == W_GUI_WIDGET_LIST_VIEW) {
			// The first cell of the picked row, formed into a string of
			// its own - none when nothing is picked.
			REBINT  n = Gui_Widget_Get_Index(wid);
			REBCNT  index = 0, cell;
			REBSER *blk = List_Items(wid, &index);
			RXIARG  val;
			if (n < 0 || !blk || wid->fields == 0) { *type = RXT_NONE; break; }
			cell = RL_GET_VALUE(blk, index + (REBCNT)n * wid->fields, &val);
			if (cell == 0 || cell == RXT_END) { *type = RXT_NONE; break; }
			str = (REBSER*)RL_MAKE_STRING(16, FALSE);
			if (!str) { *type = RXT_NONE; break; }
			if (cell != RXT_NONE && cell != RXT_UNSET) {
				RL_PROTECT_GC(str, 1);
				RL_FORM_VALUE(str, val, cell, 0, 0);
				RL_PROTECT_GC(str, 0);
			}
			arg->series = str;
			arg->index  = 0;
			*type = RXT_STRING;
			break;
		}
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST || wid->kind == W_GUI_WIDGET_DROP_LIST
		 || wid->kind == W_GUI_WIDGET_TAB_PANEL) {
			// A list box, or a non-editable drop-down, has no text of its
			// own - the picked item is it, and nothing picked reads as
			// none, not as an empty string.
			REBINT n = Gui_Widget_Get_Index(wid);
			str = (n < 0) ? NULL : Gui_Widget_Get_Item(wid, (REBCNT)n);
		}
		else str = Gui_Widget_Get_Text(wid);
		if (!str) { *type = RXT_NONE; break; }
		arg->series = str;
		arg->index  = 0;
		*type = RXT_STRING;
		break; }

	/*******************************************************************
	**  The image an image widget is showing - the very series it was
	**  given, not a copy, so drawing into it and calling `redraw` is
	**  how a widget is animated.
	**
	**  Filled in as an image and NOTHING else. RXIARG carries an image
	**  as {pointer, width:16, height:16} and a series as {pointer,
	**  index}, which puts the dimensions and the index on the same four
	**  bytes - so `index` here necessarily reads as (height<<16)|width,
	**  and the conversion behind a handle path must ignore it, exactly
	**  as it ignores it for an image passed the other way. An image!
	**  takes its size from its own series; there is nothing for an
	**  index to mean here.
	*******************************************************************/
	case W_GUI_ARG_IMAGE: {
		REBSER *img;
		if (wid->kind != W_GUI_WIDGET_IMAGE
		    || !(img = Hob_Payload(hob))) {
			*type = RXT_NONE;
			break;
		}
		CLEARS(arg);
		arg->image  = img;
		arg->width  = (int)IMG_WIDE(img);
		arg->height = (int)IMG_HIGH(img);
		*type = RXT_IMAGE;
		break; }

	case W_GUI_ARG_KIND:
		// The word list the kind was taken from is also how it is named.
		*type = RXT_WORD;
		arg->int32a = (i32)Gui_widget_words[wid->kind];
		break;

	case W_GUI_ARG_STATE:
		if (!Kind_Has_State(wid->kind)) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		// A checkbox toggles itself, so the control knows best; a radio's
		// truth is kept here, because AppKit interferes with the control.
		arg->int32a = (wid->kind == W_GUI_WIDGET_RADIO)
			? (wid->state != 0)
			: Gui_Widget_Get_State(wid);
		break;

	case W_GUI_ARG_BORDERQ:
		// A panel's frame, or the border of an entry or a list.
		if (Kind_Has_Border(wid->kind)) {
			*type = RXT_LOGIC;
			arg->int32a = Gui_Widget_Get_Border(wid) ? 1 : 0;
			break;
		}
		if (wid->kind != W_GUI_WIDGET_PANEL) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		arg->int32a = ((wid->state & GUI_PANEL_BORDER) != 0);
		break;

	/*******************************************************************
	**  Typography. All four come out of one question put to the
	**  control, so what is reported is what the control actually has -
	**  including a font this extension never set.
	*******************************************************************/
	case W_GUI_ARG_FONT:
	case W_GUI_ARG_FONT_SIZE:
	case W_GUI_ARG_BOLDQ:
	case W_GUI_ARG_ITALICQ: {
		REBSER *name = NULL;
		REBINT  size = 0;
		REBCNT  style = 0;

		if (!Kind_Has_Font(wid->kind)
		    || !Gui_Widget_Get_Font(wid, &name, &size, &style)) {
			*type = RXT_NONE;
			break;
		}
		switch (word) {
		case W_GUI_ARG_FONT:
			// None rather than an empty string: the control is using
			// whatever the platform hands out, which has no name here.
			if (!name) { *type = RXT_NONE; break; }
			arg->series = name;
			arg->index  = 0;
			*type = RXT_STRING;
			break;
		case W_GUI_ARG_FONT_SIZE:
			if (size <= 0) { *type = RXT_NONE; break; }
			*type = RXT_INTEGER;
			arg->int64 = (i64)size;
			break;
		case W_GUI_ARG_BOLDQ:
			*type = RXT_LOGIC;
			arg->int32a = ((style & GUI_FONT_BOLD) != 0);
			break;
		default: // W_GUI_ARG_ITALICQ
			*type = RXT_LOGIC;
			arg->int32a = ((style & GUI_FONT_ITALIC) != 0);
			break;
		}
		break; }

	case W_GUI_ARG_COLOR:
		// The colour is kept here rather than asked of the control, so a
		// Win32 push button - which ignores one - still reports what it
		// was given rather than pretending it was never asked.
		if (!Kind_Has_Font(wid->kind) || !GUI_COLOR_HAS(wid->color)) {
			*type = RXT_NONE;
			break;
		}
		CLEARS(arg);
		arg->tuple_len      = 3;
		arg->tuple_bytes[0] = (REBYTE)GUI_COLOR_R(wid->color);
		arg->tuple_bytes[1] = (REBYTE)GUI_COLOR_G(wid->color);
		arg->tuple_bytes[2] = (REBYTE)GUI_COLOR_B(wid->color);
		*type = RXT_TUPLE;
		break;

	case W_GUI_ARG_BACKGROUND:
		// `none` covers both "the platform's own" and "nothing at all" -
		// the second is what `transparent?` is for, and a transparent
		// widget has no colour to report.
		if (wid->kind == W_GUI_WIDGET_LIST_VIEW && GUI_LIST_STRIPED(wid)) {
			// Two colours, read back as they were given - none for the
			// platform's own.
			REBSER *blk = (REBSER*)RL_MAKE_BLOCK(2);
			RXIARG  c;
			int     i;
			if (!blk) { *type = RXT_NONE; break; }
			for (i = 0; i < 2; i++) {
				CLEARS(&c);
				if (GUI_COLOR_HAS(wid->rows[i])) {
					c.tuple_len = 3;
					c.tuple_bytes[0] = (REBYTE)GUI_COLOR_R(wid->rows[i]);
					c.tuple_bytes[1] = (REBYTE)GUI_COLOR_G(wid->rows[i]);
					c.tuple_bytes[2] = (REBYTE)GUI_COLOR_B(wid->rows[i]);
					RL_SET_VALUE(blk, (u32)i, c, RXT_TUPLE);
				} else {
					RL_SET_VALUE(blk, (u32)i, c, RXT_NONE);
				}
			}
			arg->series = blk;
			arg->index  = 0;
			*type = RXT_BLOCK;
			break;
		}
		if (!Kind_Has_Font(wid->kind) || !GUI_COLOR_HAS(wid->background)) {
			*type = RXT_NONE;
			break;
		}
		CLEARS(arg);
		arg->tuple_len      = 3;
		arg->tuple_bytes[0] = (REBYTE)GUI_COLOR_R(wid->background);
		arg->tuple_bytes[1] = (REBYTE)GUI_COLOR_G(wid->background);
		arg->tuple_bytes[2] = (REBYTE)GUI_COLOR_B(wid->background);
		*type = RXT_TUPLE;
		break;

	case W_GUI_ARG_TRANSPARENTQ:
		if (!Kind_Has_Font(wid->kind)) { *type = RXT_NONE; break; }
		arg->int32a = GUI_BG_IS_CLEAR(wid->background) ? 1 : 0;
		*type = RXT_LOGIC;
		break;

	case W_GUI_ARG_GROUP:
		*type = RXT_INTEGER;
		arg->int64 = (i64)wid->group;
		break;

	case W_GUI_ARG_ITEMS: {
		REBSER *blk;
		if (wid->kind == W_GUI_WIDGET_TREE_VIEW) {
			// The very block it was given, as a list-view's.
			REBCNT index = 0;
			blk = List_Items(wid, &index);
			if (!blk) { *type = RXT_NONE; break; }
			arg->series = blk;
			arg->index  = index;
			*type = RXT_BLOCK;
			break;
		}
		if (!Kind_Has_Items(wid->kind)) { *type = RXT_NONE; break; }
		if (wid->kind == W_GUI_WIDGET_LIST_VIEW) {
			// The very block it was given - see List_Items().
			REBCNT index = 0;
			blk = List_Items(wid, &index);
			if (!blk) { *type = RXT_NONE; break; }
			arg->series = blk;
			arg->index  = index;
			*type = RXT_BLOCK;
			break;
		}
		blk = Items_To_Block(wid);
		if (!blk) { *type = RXT_NONE; break; }
		arg->series = blk;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	case W_GUI_ARG_INDEX: {
		REBINT n;
		if (!Kind_Has_Items(wid->kind)) { *type = RXT_NONE; break; }
		n = Gui_Widget_Get_Index(wid);
		*type = RXT_INTEGER;
		// Rebol counts from one, and zero means nothing is picked.
		arg->int64 = (i64)(n < 0 ? 0 : n + 1);
		break; }

	case W_GUI_ARG_VALUE:
		if (wid->kind == W_GUI_WIDGET_DATE_FIELD) {
			GUIDATE d;
			if (!Gui_Widget_Get_Date(wid, &d)) { *type = RXT_NONE; break; }
			*type = RXT_DATE;
			arg->datetime.date = (i32)Bits_From_Date(&d);
			// A field without `/time` has no time of day to report, and
			// reads as a plain date.
			arg->datetime.time = (wid->state & GUI_DATE_TIME) ? (i64)d.ns : NO_TIME;
			break;
		}
		if (!Kind_Has_Value(wid->kind)) { *type = RXT_NONE; break; }
		// Reported as a percent!, which is what a fraction of a range
		// reads as in Rebol - `50%` rather than `0.5`.
		*type = RXT_PERCENT;
		arg->dec64 = (double)Gui_Widget_Get_Value(wid);
		break;

	// A fraction of the way down, as a percent! - the same way a slider
	// reports its position, because it is the same kind of answer.
	case W_GUI_ARG_SCROLL: {
		REBDEC at;
		if (!Kind_Scrolls(wid->kind)) { *type = RXT_NONE; break; }
		at = Gui_Widget_Get_Scroll(wid);
		if (at < 0.0) { *type = RXT_NONE; break; }
		*type = RXT_PERCENT;
		arg->dec64 = (double)at;
		break; }

	case W_GUI_ARG_SECUREQ:
		if (wid->kind != W_GUI_WIDGET_FIELD) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		arg->int32a = (wid->state & GUI_TEXT_SECURE) ? 1 : 0;
		break;

	case W_GUI_ARG_SCROLLABLEQ:
		if (wid->kind != W_GUI_WIDGET_TEXT_LIST) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		arg->int32a = (wid->state & GUI_LIST_FIXED) ? 0 : 1;
		break;

	case W_GUI_ARG_COLUMNS: {
		REBSER *blk;
		if (wid->kind != W_GUI_WIDGET_LIST_VIEW) { *type = RXT_NONE; break; }
		blk = List_Columns_Block(wid);
		if (!blk) { *type = RXT_NONE; break; }
		arg->series = blk;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	case W_GUI_ARG_SELECTED:
		if (wid->kind != W_GUI_WIDGET_TREE_VIEW) { *type = RXT_NONE; break; }
		if (!Tree_Value(wid, Gui_Tree_Selected(wid), arg, type)) *type = RXT_NONE;
		break;

	case W_GUI_ARG_NODES: {
		REBSER *blk;
		if (wid->kind != W_GUI_WIDGET_TREE_VIEW) { *type = RXT_NONE; break; }
		blk = Tree_Nodes(wid);
		if (!blk) { *type = RXT_NONE; break; }
		arg->series = blk;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	case W_GUI_ARG_EXPANDED: {
		REBSER *blk;
		if (wid->kind != W_GUI_WIDGET_TREE_VIEW) { *type = RXT_NONE; break; }
		blk = Tree_Expanded(wid);
		if (!blk) { *type = RXT_NONE; break; }
		arg->series = blk;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	case W_GUI_ARG_SORT_COLUMN:
		if (wid->kind != W_GUI_WIDGET_LIST_VIEW || wid->sort == 0) { *type = RXT_NONE; break; }
		*type = RXT_INTEGER;
		arg->int64 = (i64)wid->sort;
		break;

	case W_GUI_ARG_SIZE:
		if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) { *type = RXT_NONE; break; }
		arg->pair.x = (float)w;
		arg->pair.y = (float)h;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_OFFSET:
		if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) { *type = RXT_NONE; break; }
		arg->pair.x = (float)x;
		arg->pair.y = (float)y;
		*type = RXT_PAIR;
		break;

	// Where it sits in its WINDOW rather than in its container. Mouse
	// events report window client coordinates, so `evt/offset - canvas/at`
	// is where on the canvas. Asked of the backend: a sum of `offset`s up
	// the chain is only right where every container places its children
	// itself - an NSTabView puts a page inside views of its own. See
	// Gui_Widget_Get_At().
	case W_GUI_ARG_AT:
		if (!Gui_Widget_Get_At(wid, &x, &y)) { *type = RXT_NONE; break; }
		arg->pair.x = (float)x;
		arg->pair.y = (float)y;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_ID:
		*type = RXT_INTEGER;
		arg->int64 = (i64)(REBUPT)wid->handle;
		break;

	// Asked of the platform, not kept here: focus moves for reasons this
	// extension never hears about - a click, the window being activated,
	// another application taking over - so a remembered flag would drift.
	case W_GUI_ARG_FOCUSEDQ:
		*type = RXT_LOGIC;
		arg->int32a = (wid->handle
		            && Kind_Takes_Focus(wid->kind)
		            && Gui_Widget_Has_Focus(wid)) ? 1 : 0;
		break;

	// Read from the widget rather than the control: see the note in gui.h
	// on why the flag is kept here.
	case W_GUI_ARG_READ_ONLYQ:
		if (!Kind_Has_Read_Only(wid->kind)) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		arg->int32a = (wid->state & GUI_TEXT_READ_ONLY) ? 1 : 0;
		break;

	// Asked of the control, like the font: what is reported is what the
	// platform will show. None when there is no tip.
	case W_GUI_ARG_TIP: {
		REBSER *tip = Gui_Widget_Get_Tip(wid);
		if (!tip) { *type = RXT_NONE; break; }
		arg->series = tip;
		arg->index  = 0;
		*type = RXT_STRING;
		break; }

	case W_GUI_ARG_ENABLEDQ:
		if (!Kind_Has_Enabled(wid->kind)) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		arg->int32a = Gui_Widget_Get_Enabled(wid);
		break;

	// Whatever holds it: a panel if it is in one, otherwise the window.
	// Only a container has any; everything else answers none, which is
	// how a caller can tell the two apart without a list of kinds.
	case W_GUI_ARG_CHILDREN: {
		REBSER *kids;
		if (!Kind_Is_Container(wid->kind)) { *type = RXT_NONE; break; }
		kids = Hob_Children(hob, FALSE);
		if (!kids) kids = (REBSER*)RL_MAKE_BLOCK(0);
		if (!kids) { *type = RXT_NONE; break; }
		arg->series = kids;
		arg->index  = 0;
		*type = RXT_BLOCK;
		break; }

	case W_GUI_ARG_PARENT:
		if (wid->parent && ((GUIWIDGET*)wid->parent)->hob) {
			Set_Handle_Arg(arg, ((GUIWIDGET*)wid->parent)->hob);
			*type = RXT_HANDLE;
			break;
		}
		// fall through - no panel means the window holds it
	case W_GUI_ARG_WINDOW:
		if (!wid->owner || !wid->owner->hob) { *type = RXT_NONE; break; }
		Set_Handle_Arg(arg, wid->owner->hob);
		*type = RXT_HANDLE;
		break;

	default:
		return PE_BAD_SELECT;
	}
	return PE_USE;
}


int GuiWidget_set_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg)
{
	GUIWIDGET *wid = (GUIWIDGET*)hob->data;
	REBINT x, y, w, h;

	if (!wid->handle) return PE_BAD_SET;

	// Translated once, here, rather than inside the switch: a case which
	// looks at `word` itself (bold? and italic? share one) must see the
	// extension's own index, not the raw symbol - which never equals any
	// W_GUI_ARG_* and made `bold?: true` set ITALIC instead.
	word = RL_FIND_WORD(Gui_arg_words, word);

	switch (word) {
	case W_GUI_ARG_ITEMS:
		if (wid->kind == W_GUI_WIDGET_TREE_VIEW) {
			if (*type == RXT_NONE) { Tree_Set_Items(wid, NULL, 0); break; }
			if (*type != RXT_BLOCK) return PE_BAD_SET_TYPE;
			if (!Tree_Set_Items(wid, (REBSER*)arg->series, arg->index)) return PE_BAD_SET;
			break;
		}
		if (!Kind_Has_Items(wid->kind)) return PE_BAD_SET;
		if (wid->kind == W_GUI_WIDGET_LIST_VIEW) {
			if (*type == RXT_NONE) { List_Set_Items(wid, NULL, 0); break; }
			if (*type != RXT_BLOCK) return PE_BAD_SET_TYPE;
			if (!List_Set_Items(wid, (REBSER*)arg->series, arg->index)) return PE_BAD_SET;
			break;
		}
		// A tab-panel's labels come with pages, which scripts hold on to;
		// replacing them is not supported (yet).
		if (wid->kind == W_GUI_WIDGET_TAB_PANEL) return PE_BAD_SET;
		if (*type != RXT_BLOCK) return PE_BAD_SET_TYPE;
		Block_To_Items(wid, (REBSER*)arg->series);
		break;

	case W_GUI_ARG_INDEX:
		if (!Kind_Has_Items(wid->kind)) return PE_BAD_SET;
		if (*type != RXT_INTEGER) return PE_BAD_SET_TYPE;
		// Out of range - zero included - simply picks nothing.
		if (wid->kind == W_GUI_WIDGET_LIST_VIEW) List_Set_Index(wid, (REBINT)arg->int64 - 1);
		else Gui_Widget_Set_Index(wid, (REBINT)arg->int64 - 1);
		break;

	case W_GUI_ARG_TEXT: {
		REBYTE *utf8 = NULL;
		int len;
		if (!Kind_Has_Text(wid->kind)) return PE_BAD_SET;
		// A drop-list or a text-list shows whichever item is picked;
		// `index` chooses it. A drop-down is the one list-like kind
		// whose text CAN be set - it is the typed value, not a pick.
		if (wid->kind == W_GUI_WIDGET_DROP_LIST
		 || wid->kind == W_GUI_WIDGET_TEXT_LIST
		 || wid->kind == W_GUI_WIDGET_LIST_VIEW
		 || wid->kind == W_GUI_WIDGET_TAB_PANEL) return PE_BAD_SET;
		if (*type != RXT_STRING) return PE_BAD_SET_TYPE;
		len = RL_GET_UTF8_STRING((REBSER*)arg->series, arg->index, (void**)&utf8);
		if (len < 0) return PE_BAD_SET;
		Gui_Widget_Set_Text(wid, utf8, (REBCNT)len);
		// A native control repaints itself when its text changes; a panel's
		// caption is drawn by the backend's own paint handler, so it has to
		// be asked.
		if (wid->kind == W_GUI_WIDGET_PANEL) Gui_Panel_Border_Changed(wid);
		break; }

	// Swapping the image is just swapping the reference the GC marks; the
	// widget keeps its box and the new image is scaled into it.
	case W_GUI_ARG_IMAGE:
		if (wid->kind != W_GUI_WIDGET_IMAGE) return PE_BAD_SET;
		if (*type != RXT_IMAGE) return PE_BAD_SET_TYPE;
		if (!arg->image) return PE_BAD_SET;
		if (!Hob_Set_Payload(hob, arg, RXT_IMAGE)) return PE_BAD_SET;
		// Invalidated, not painted - `redraw` is the one thing that still
		// promises pixels on screen before it returns, and everything else
		// waits for the pump.
		Gui_Widget_Invalidate(wid);
		break;

	// Both halves of the box are read back first, so that setting one does
	// not disturb the other.
	/*******************************************************************
	**  A ZERO AXIS ASKS THE WIDGET, exactly as at creation - which is
	**  the whole of the re-fit interface:
	**
	**      lbl/size: 220x0   ;; keep the width, measure the height
	**      lbl/size: 0x0     ;; measure both
	**
	**  Nothing re-measures on its own, so this is what a script calls
	**  after changing a font or a label, when it wants the box to follow
	**  and has decided there is room for it.
	*******************************************************************/
	case W_GUI_ARG_SIZE: {
		REBOOL fit_w, fit_h;
		if (*type != RXT_PAIR) return PE_BAD_SET_TYPE;
		if (arg->pair.x < 0 || arg->pair.y < 0) return PE_BAD_RANGE;
		if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) return PE_BAD_SET;

		fit_w = arg->pair.x == 0 ? TRUE : FALSE;
		fit_h = arg->pair.y == 0 ? TRUE : FALSE;

		// The given axes first, so that measuring the other one happens
		// against the box the caller is asking for.
		if (!fit_w) w = (REBINT)arg->pair.x;
		if (!fit_h) h = (REBINT)arg->pair.y;
		Gui_Widget_Set_Box(wid, x, y, w, h);

		// An image or a panel has no size of its own to report, so asking
		// one is refused rather than quietly ignored.
		if (!Fit_To_Content(wid, fit_w, fit_h)) return PE_BAD_SET;
		break; }

	case W_GUI_ARG_OFFSET:
		if (*type != RXT_PAIR) return PE_BAD_SET_TYPE;
		if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) return PE_BAD_SET;
		Gui_Widget_Set_Box(wid, (REBINT)arg->pair.x, (REBINT)arg->pair.y, w, h);
		break;

	case W_GUI_ARG_STATE:
		if (!Kind_Has_State(wid->kind)) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (wid->kind == W_GUI_WIDGET_RADIO) {
			// Setting one from Rebol settles the group exactly as a click
			// does - including turning the others off.
			Sync_Radio_Group(wid, arg->int32a ? TRUE : FALSE);
		} else {
			wid->state = arg->int32a ? 1 : 0;
			Gui_Widget_Set_State(wid, wid->state ? TRUE : FALSE);
		}
		break;

	/*******************************************************************
	**  Typography. Each of the four writes one part of a font which is
	**  otherwise carried over from the control - see Set_Font_Part.
	*******************************************************************/
	case W_GUI_ARG_FONT: {
		REBYTE *utf8 = NULL;
		int     len = 0;
		if (!Kind_Has_Font(wid->kind)) return PE_BAD_SET;
		// `none` means "the platform's own font", which is what a NULL
		// name asks a backend for.
		if (*type == RXT_STRING) {
			len = RL_GET_UTF8_STRING((REBSER*)arg->series, arg->index,
			                         (void**)&utf8);
			if (len < 0) return PE_BAD_SET;
		} else if (*type != RXT_NONE) {
			return PE_BAD_SET_TYPE;
		}
		if (!Set_Font_Part(wid, FONT_PART_NAME, utf8, (REBCNT)len, 0, 0, 0))
			return PE_BAD_SET;
		break; }

	case W_GUI_ARG_FONT_SIZE: {
		REBINT size;
		if (!Kind_Has_Font(wid->kind)) return PE_BAD_SET;
		if (*type == RXT_NONE) {
			size = 0; // back to the platform's own size
		} else if (*type == RXT_INTEGER) {
			size = (REBINT)arg->int64;
			if (size <= 0 || size > 1000) return PE_BAD_RANGE;
		} else {
			return PE_BAD_SET_TYPE;
		}
		if (!Set_Font_Part(wid, FONT_PART_SIZE, NULL, 0, size, 0, 0))
			return PE_BAD_SET;
		break; }

	case W_GUI_ARG_BOLDQ:
	case W_GUI_ARG_ITALICQ: {
		REBCNT mask = (word == W_GUI_ARG_BOLDQ)
			? GUI_FONT_BOLD : GUI_FONT_ITALIC;
		if (!Kind_Has_Font(wid->kind)) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (!Set_Font_Part(wid, FONT_PART_STYLE, NULL, 0, 0,
		                   arg->int32a ? mask : 0, mask))
			return PE_BAD_SET;
		break; }

	case W_GUI_ARG_COLOR:
		if (!Kind_Has_Font(wid->kind)) return PE_BAD_SET;
		if (*type == RXT_NONE) {
			wid->color = 0; // the platform decides again
		} else if (*type == RXT_TUPLE) {
			if (arg->tuple_len < 3) return PE_BAD_SET;
			// A fourth byte would be alpha, which no control here blends.
			wid->color = GUI_COLOR_OF(arg->tuple_bytes[0],
			                          arg->tuple_bytes[1],
			                          arg->tuple_bytes[2]);
		} else {
			return PE_BAD_SET_TYPE;
		}
		// A backend which cannot colour this particular control says so,
		// and the value stands anyway - the accessor reports what was
		// asked for, and the one platform gap is documented rather than
		// turned into an error at an arbitrary moment.
		Gui_Widget_Set_Color(wid);
		break;

	case W_GUI_ARG_BACKGROUND:
		if (!Kind_Has_Font(wid->kind)) return PE_BAD_SET;
		// One colour, or none, takes the stripes off a list-view.
		wid->rows[0] = wid->rows[1] = 0;
		if (*type == RXT_BLOCK && wid->kind == W_GUI_WIDGET_LIST_VIEW) {
			// Two colours: the rows alternate - the even ones first. Either
			// may be none, the platform's own, which is what makes the
			// stripes start on the first row or on the second.
			RXIARG c;
			REBCNT t;
			int    i;
			t = RL_GET_VALUE((REBSER*)arg->series, arg->index + 2, &c);
			if (t != 0 && t != RXT_END) return PE_BAD_SET;   // more than two
			for (i = 0; i < 2; i++) {
				t = RL_GET_VALUE((REBSER*)arg->series, arg->index + (u32)i, &c);
				if (t == RXT_TUPLE && c.tuple_len >= 3)
					wid->rows[i] = GUI_COLOR_OF(c.tuple_bytes[0], c.tuple_bytes[1],
					                            c.tuple_bytes[2]);
				else if (!(t == RXT_NONE || Is_None_Width(t, &c))) {
					wid->rows[0] = wid->rows[1] = 0;
					return PE_BAD_SET;
				}
			}
			// The empty part below the rows: the even rows' colour when
			// both have one - it continues the list - and otherwise the
			// platform's, since a colour there would be a third stripe.
			wid->background = (GUI_COLOR_HAS(wid->rows[0]) && GUI_COLOR_HAS(wid->rows[1]))
			                ? wid->rows[0] : 0;
		} else if (*type == RXT_NONE) {
			wid->background = 0; // the platform decides again
		} else if (*type == RXT_TUPLE) {
			if (arg->tuple_len < 3) return PE_BAD_SET;
			// Giving a colour is also how transparency is turned off: a
			// widget cannot both fill with something and show through.
			wid->background = GUI_COLOR_OF(arg->tuple_bytes[0],
			                               arg->tuple_bytes[1],
			                               arg->tuple_bytes[2]);
		} else {
			return PE_BAD_SET_TYPE;
		}
		Gui_Widget_Set_Background(wid);
		break;

	case W_GUI_ARG_TRANSPARENTQ:
		if (!Kind_Has_Font(wid->kind)) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		// Turning it off goes back to the platform's own background rather
		// than to a colour set earlier: one field holds both, because the
		// two are answers to the same question.
		wid->background = arg->int32a ? GUI_BG_CLEAR : 0;
		wid->rows[0] = wid->rows[1] = 0;
		Gui_Widget_Set_Background(wid);
		break;

	case W_GUI_ARG_BORDERQ:
		// The backends read this flag at paint time, so turning a frame on
		// or off is a repaint and never a rebuild of the control - which is
		// also why nothing the panel holds moves.
		if (Kind_Has_Border(wid->kind)) {
			if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
			Gui_Widget_Set_Border(wid, arg->int32a ? TRUE : FALSE);
			break;
		}
		if (wid->kind != W_GUI_WIDGET_PANEL) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) wid->state |=  GUI_PANEL_BORDER;
		else             wid->state &= ~(REBCNT)GUI_PANEL_BORDER;
		Gui_Panel_Border_Changed(wid);
		break;

	case W_GUI_ARG_VALUE: {
		REBDEC value;
		if (wid->kind == W_GUI_WIDGET_DATE_FIELD) {
			GUIDATE d;
			if (*type != RXT_DATE) return PE_BAD_SET_TYPE;
			if (!Date_From_Arg(wid, (u32)arg->datetime.date, arg->datetime.time, &d))
				return PE_BAD_SET;
			Gui_Widget_Set_Date(wid, &d);
			break;
		}
		if (!Kind_Has_Value(wid->kind)) return PE_BAD_SET;
		// A percent! is a decimal! underneath, so both arrive the same way.
		if (*type != RXT_PERCENT && *type != RXT_DECIMAL) return PE_BAD_SET_TYPE;
		value = (REBDEC)arg->dec64;
		if (value < 0.0) value = 0.0;
		if (value > 1.0) value = 1.0;
		Gui_Widget_Set_Value(wid, value);
		break; }

	/*******************************************************************
	**  A percent, or one of the words - which is the whole reason the
	**  two forms are both here. `100%` is what a computed position
	**  looks like; `'bottom` is what the call site usually means, and
	**  saying so beats a magic number that has to be recognised.
	*******************************************************************/
	case W_GUI_ARG_SCROLL: {
		REBDEC where;

		if (!Kind_Scrolls(wid->kind)) return PE_BAD_SET;

		if (*type == RXT_WORD) {
			switch (RL_FIND_WORD(Gui_scroll_words, (REBCNT)arg->int32a)) {
			case W_GUI_SCROLL_TOP:    where = 0.0; break;
			// `end` is `bottom` under the name that reads better after
			// appending to something.
			case W_GUI_SCROLL_BOTTOM:
			case W_GUI_SCROLL_END:    where = 1.0; break;
			default: return PE_BAD_SET;
			}
		} else if (*type == RXT_INTEGER && (wid->kind == W_GUI_WIDGET_TEXT_LIST
		                                 || wid->kind == W_GUI_WIDGET_LIST_VIEW)) {
			// An item, 1-based like `index`, brought into view without
			// being picked. Clamped like a percent: 0 or less is the first
			// item, past the end is the last.
			REBCNT count = Gui_Widget_Count_Items(wid);
			i64    n     = arg->int64;
			if (count == 0) break;
			if (n < 1) n = 1;
			if (n > (i64)count) n = (i64)count;
			Gui_Widget_Scroll_To_Item(wid, (REBINT)(n - 1));
			break;
		} else if (*type == RXT_PERCENT || *type == RXT_DECIMAL) {
			where = (REBDEC)arg->dec64;
			if (where < 0.0) where = 0.0;
			if (where > 1.0) where = 1.0;
		} else {
			return PE_BAD_SET_TYPE;
		}

		Gui_Widget_Set_Scroll(wid, where);
		break; }

	// A path, a word or a label; none picks nothing. One that names no
	// node picks nothing either, rather than erroring: the tree may just
	// have been replaced.
	case W_GUI_ARG_SELECTED:
		if (wid->kind != W_GUI_WIDGET_TREE_VIEW) return PE_BAD_SET;
		if (*type == RXT_NONE) { Tree_Select(wid, -1); break; }
		if (*type != RXT_PATH && *type != RXT_BLOCK
		 && *type != RXT_WORD && *type != RXT_STRING)
			return PE_BAD_SET_TYPE;
		Tree_Select(wid, Tree_Find(wid, *type, arg));
		break;

	case W_GUI_ARG_EXPANDED:
		if (wid->kind != W_GUI_WIDGET_TREE_VIEW) return PE_BAD_SET;
		if (*type != RXT_BLOCK) return PE_BAD_SET_TYPE;
		Tree_Set_Expanded(wid, (REBSER*)arg->series, arg->index);
		break;

	case W_GUI_ARG_COLUMNS:
		if (wid->kind != W_GUI_WIDGET_LIST_VIEW) return PE_BAD_SET;
		if (*type != RXT_BLOCK) return PE_BAD_SET_TYPE;
		if (!List_Set_Columns(wid, (REBSER*)arg->series, arg->index)) return PE_BAD_SET;
		break;

	case W_GUI_ARG_SORT_COLUMN: {
		i64 n;
		if (wid->kind != W_GUI_WIDGET_LIST_VIEW) return PE_BAD_SET;
		if (*type == RXT_NONE) n = 0;
		else if (*type == RXT_INTEGER) n = arg->int64;
		else return PE_BAD_SET_TYPE;
		// A column that is not there shows no arrow.
		if (n > (i64)wid->fields || -n > (i64)wid->fields) n = 0;
		wid->sort = (REBINT)n;
		Gui_List_Show_Sort(wid);
		break; }

	case W_GUI_ARG_SCROLLABLEQ:
		if (wid->kind != W_GUI_WIDGET_TEXT_LIST) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		if (arg->int32a) wid->state &= ~GUI_LIST_FIXED;
		else             wid->state |=  GUI_LIST_FIXED;
		Gui_Widget_Set_Scrollable(wid, arg->int32a ? TRUE : FALSE);
		break;

	case W_GUI_ARG_ENABLEDQ:
		if (!Kind_Has_Enabled(wid->kind)) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		Gui_Widget_Set_Enabled(wid, arg->int32a ? TRUE : FALSE);
		break;

	// Any kind can have one. None, or an empty string, takes it away.
	case W_GUI_ARG_TIP: {
		REBYTE *utf8 = NULL;
		int     len  = 0;
		if (*type == RXT_STRING) {
			len = RL_GET_UTF8_STRING((REBSER*)arg->series, arg->index, (void**)&utf8);
			if (len < 0) return PE_BAD_SET;
		} else if (*type != RXT_NONE) {
			return PE_BAD_SET_TYPE;
		}
		if (!Gui_Widget_Set_Tip(wid, utf8, (REBCNT)len)) return PE_BAD_SET;
		break; }

	case W_GUI_ARG_READ_ONLYQ:
		if (!Kind_Has_Read_Only(wid->kind)) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		// Recorded before the backend is told, because that is where it
		// reads the answer from when it combines this with `enabled?`.
		if (arg->int32a) wid->state |=  GUI_TEXT_READ_ONLY;
		else             wid->state &= ~(REBCNT)GUI_TEXT_READ_ONLY;
		Gui_Widget_Set_Read_Only(wid, arg->int32a ? TRUE : FALSE);
		break;

	default:
		return PE_BAD_SET;
	}
	return PE_OK;
}


int GuiWidget_mold(REBHOB *hob, REBSER *str)
{
	int len;
	GUIWIDGET *wid;

	if (!str || !hob || !(wid = (GUIWIDGET*)hob->data)) return 0;

	SERIES_TAIL(str) = 0;
	if (wid->handle) {
		APPEND_STRING(str, "0#%lx %s", (unsigned long)(REBUPT)wid->handle,
			Kind_Name(wid->kind));
	} else {
		APPEND_STRING(str, "%s", "removed");
	}
	return len;
}


//== drop handle ==============================================================
//
// What a `drop-file` or a `drop-text` event carries as its source. It owns
// nothing outside hob->series, and nothing about it can be set - a drop is
// something that happened, not something to configure - so there is no
// set_path and the free callback has nothing to release.

int GuiDrop_free(void *hndl)
{
	REBHOB  *hob  = (REBHOB*)hndl;
	GUIDROP *drop = hob ? (GUIDROP*)hob->data : NULL;

	// The content and the target are Rebol values in hob->series, which the
	// collector handles on its own. Clearing is only tidiness.
	if (drop) CLEARS(drop);
	if (hob) UNMARK_HOB(hob);
	return 0;
}


int GuiDrop_get_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg)
{
	GUIDROP *drop = (GUIDROP*)hob->data;
	REBSER  *slots = hob->series;

	if (!drop || !slots) return PE_BAD_SELECT;

	switch (RL_FIND_WORD(Gui_arg_words, word)) {

	case W_GUI_ARG_KIND:
		*type = RXT_WORD;
		arg->int32a = (i32)Gui_drop_words[drop->kind];
		break;

	case W_GUI_ARG_DATA:
		// Whatever was put in slot 0 - a block of file!, or a string.
		*type = RL_GET_VALUE(slots, SLOT_PAYLOAD, arg);
		break;

	case W_GUI_ARG_COUNT:
		*type = RXT_INTEGER;
		arg->int64 = (i64)drop->count;
		break;

	case W_GUI_ARG_TARGET:
		*type = RL_GET_VALUE(slots, SLOT_CHILDREN, arg);
		break;

	case W_GUI_ARG_WINDOW: {
		// The target's own window, asked of the target - a drop on a widget
		// reports the widget, and this is how to get from it to the window.
		RXIARG   val;
		REBCNT   t = RL_GET_VALUE(slots, SLOT_CHILDREN, &val);
		REBHOB  *target;

		if (t != RXT_HANDLE || !(target = val.handle.hob)) { *type = RXT_NONE; break; }
		if (target->sym == Handle_GuiWindow) { *arg = val; *type = RXT_HANDLE; break; }
		if (target->sym == Handle_GuiWidget) {
			GUIWIDGET *wid = (GUIWIDGET*)target->data;
			if (wid && wid->owner && wid->owner->hob) {
				Set_Handle_Arg(arg, wid->owner->hob);
				*type = RXT_HANDLE;
				break;
			}
		}
		*type = RXT_NONE;
		break;
	}

	default:
		return PE_BAD_SELECT;
	}
	return PE_USE;
}


int GuiDrop_mold(REBHOB *hob, REBSER *str)
{
	int len;
	GUIDROP *drop;

	if (!str || !hob || !(drop = (GUIDROP*)hob->data)) return 0;

	SERIES_TAIL(str) = 0;
	APPEND_STRING(str, "%s %u",
		(drop->kind == GUI_DROP_TEXT) ? "text" : "files", drop->count);
	return len;
}


//== screens ==================================================================
//
// One handle per display, reused: `screens` and `win/screen` hand back the
// SAME handle for the same display while anything still holds it, so `==`
// answers "is this the same screen?" as it does for windows and widgets.
//
// The table only remembers; it does not keep anything alive. A screen
// handle nobody holds is collected like any other value, and its free
// callback takes it out of the table - so a slot can never point at a
// context the collector has reused.
//
// Sixteen displays is far beyond anything real. A seventeenth still gets a
// handle; it just is not shared, so `==` on two reads of it says false.

#define SCREEN_CACHE 16
static REBHOB *Screen_Cache[SCREEN_CACHE];

static REBHOB *Screen_Handle(const REBYTE *key)
{
	REBHOB    *hob;
	GUISCREEN *scr;
	REBCNT     n, free_slot = SCREEN_CACHE;

	if (!key || !key[0]) return NULL;

	for (n = 0; n < SCREEN_CACHE; n++) {
		hob = Screen_Cache[n];
		if (!hob) { if (free_slot == SCREEN_CACHE) free_slot = n; continue; }
		scr = (GUISCREEN*)hob->data;
		if (IS_USED_HOB(hob) && scr
		    && strncmp((const char*)scr->key, (const char*)key, GUI_SCREEN_KEY) == 0)
			return hob;
	}

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiScreen);
	if (!hob) return NULL;
	scr = (GUISCREEN*)hob->data;
	scr->hob = hob;
	strncpy((char*)scr->key, (const char*)key, GUI_SCREEN_KEY - 1);
	scr->key[GUI_SCREEN_KEY - 1] = 0;

	// Making the handle may have collected - and a collected screen handle
	// empties its own slot - so the free slot is looked for again.
	for (n = 0; n < SCREEN_CACHE; n++) {
		if (!Screen_Cache[n]) { Screen_Cache[n] = hob; break; }
	}
	return hob;
}


/***********************************************************************
**  screens
**
**  One handle per connected display, primary first. Asked of the
**  platform every time, so a display plugged in since the last call is
**  there and one taken away is not.
***********************************************************************/
COMMAND cmd_gui_screens(RXIFRM *frm, void *ctx)
{
	REBYTE  keys[SCREEN_CACHE][GUI_SCREEN_KEY];
	REBCNT  count, n, at = 0;
	REBSER *blk;
	RXIARG  val;

	count = Gui_Screen_Keys(keys, SCREEN_CACHE);
	if (count > SCREEN_CACHE) count = SCREEN_CACHE;

	blk = (REBSER*)RL_MAKE_BLOCK(count);
	if (!blk) RETURN_ERROR(ERR_NO_HANDLE);

	// Each handle made below can collect; the block is what keeps the ones
	// already made alive, so it is protected until it is in the frame.
	RL_PROTECT_GC(blk, 1);
	for (n = 0; n < count; n++) {
		REBHOB *hob = Screen_Handle(keys[n]);
		if (!hob) continue;
		Set_Handle_Arg(&val, hob);
		RL_SET_VALUE(blk, at++, val, RXT_HANDLE);
	}
	RL_PROTECT_GC(blk, 0);

	RXA_SERIES(frm, 1) = blk;
	RXA_INDEX(frm, 1) = 0;
	RXA_TYPE(frm, 1) = RXT_BLOCK;
	return RXR_VALUE;
}


//== screen handle callbacks ==================================================

int GuiScreen_free(void *hndl)
{
	REBHOB    *hob = (REBHOB*)hndl;
	GUISCREEN *scr = hob ? (GUISCREEN*)hob->data : NULL;
	REBCNT     n;

	for (n = 0; n < SCREEN_CACHE; n++) {
		if (Screen_Cache[n] == hob) Screen_Cache[n] = NULL;
	}
	if (scr) CLEARS(scr);
	if (hob) UNMARK_HOB(hob);
	return 0;
}


int GuiScreen_get_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg)
{
	GUISCREEN    *scr = (GUISCREEN*)hob->data;
	GUISCREENINFO info;

	if (!scr) return PE_BAD_SELECT;

	word = RL_FIND_WORD(Gui_arg_words, word);

	// A word this extension does not know is refused, so that `scr/type`
	// still reaches the core's own answer - see GuiWindow_get_path.
	switch (word) {
	case W_GUI_ARG_NAME:
	case W_GUI_ARG_SIZE:
	case W_GUI_ARG_OFFSET:
	case W_GUI_ARG_WORK_SIZE:
	case W_GUI_ARG_WORK_OFFSET:
	case W_GUI_ARG_SCALE:
	case W_GUI_ARG_PRIMARYQ:
		break;
	default:
		return PE_BAD_SELECT;
	}

	// A display which has gone answers none for everything, the way a
	// closed window does - the handle itself stays valid.
	CLEARS(&info);
	if (!Gui_Screen_Info(scr->key, &info)) {
		*type = RXT_NONE;
		return PE_USE;
	}

	switch (word) {
	case W_GUI_ARG_NAME: {
		REBSER *name = Gui_Screen_Name(scr->key);
		if (!name) { *type = RXT_NONE; break; }
		arg->series = name;
		arg->index  = 0;
		*type = RXT_STRING;
		break; }

	case W_GUI_ARG_SIZE:
		arg->pair.x = (float)info.w;
		arg->pair.y = (float)info.h;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_OFFSET:
		arg->pair.x = (float)info.x;
		arg->pair.y = (float)info.y;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_WORK_SIZE:
		arg->pair.x = (float)info.ww;
		arg->pair.y = (float)info.wh;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_WORK_OFFSET:
		arg->pair.x = (float)info.wx;
		arg->pair.y = (float)info.wy;
		*type = RXT_PAIR;
		break;

	case W_GUI_ARG_SCALE:
		arg->dec64 = (double)info.scale;
		*type = RXT_DECIMAL;
		break;

	case W_GUI_ARG_PRIMARYQ:
		arg->int32a = info.primary ? 1 : 0;
		*type = RXT_LOGIC;
		break;
	}
	return PE_USE;
}


int GuiScreen_mold(REBHOB *hob, REBSER *str)
{
	int len;
	GUISCREEN    *scr;
	GUISCREENINFO info;

	if (!str || !hob || !(scr = (GUISCREEN*)hob->data)) return 0;

	SERIES_TAIL(str) = 0;
	CLEARS(&info);
	if (Gui_Screen_Info(scr->key, &info)) {
		APPEND_STRING(str, "%dx%d at %dx%d%s", info.w, info.h, info.x, info.y,
		              info.primary ? " primary" : "");
	} else {
		APPEND_STRING(str, "%s", "gone");
	}
	return len;
}


/***********************************************************************
**  track-mouse on [logic!]
**
**  Turns reporting of moves outside this program's windows on or off,
**  and returns whether it was on. Off by default: with it on, every
**  movement anywhere on the desktop wakes WAIT.
***********************************************************************/
COMMAND cmd_gui_track_mouse(RXIFRM *frm, void *ctx)
{
	REBOOL was = Track_Pointer;

	Track_Pointer = RXA_LOGIC(frm, 1) ? TRUE : FALSE;
	Tracked_Key[0] = 0;   // report the current position at once

	// Nothing will report the pointer leaving a screen any more, so it
	// is left now rather than whenever it next reaches a window.
	if (!Track_Pointer && !Hover.hob && Hover.screen[0]) Gui_Pointer_Left();

	RXA_LOGIC(frm, 1) = was ? 1 : 0;
	RXA_TYPE(frm, 1)  = RXT_LOGIC;
	return RXR_VALUE;
}