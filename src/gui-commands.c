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

static const REBYTE* ERR_INVALID_HANDLE = (const REBYTE*)"Invalid GUI window handle!";
static const REBYTE* ERR_NO_HANDLE      = (const REBYTE*)"Failed to create the window handle!";
static const REBYTE* ERR_NO_WINDOW      = (const REBYTE*)"Failed to create the window!";
static const REBYTE* ERR_BAD_SIZE       = (const REBYTE*)"Size must be positive!";
static const REBYTE* ERR_NO_WIDGET      = (const REBYTE*)"Failed to create the widget!";
static const REBYTE* ERR_BAD_IMAGE      = (const REBYTE*)"Empty or invalid image!";


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
**  Consecutive `move` events of the same window are collapsed onto the
**  last one: a fast mouse produces hundreds of them per second and only
**  the newest position is of any use.
***********************************************************************/
void Gui_Queue_Event(REBHOB *source, REBCNT type, REBINT x, REBINT y, REBINT value)
{
	GUIEVT *evt;

	if (type == W_GUI_EVENT_MOVE && QUEUE_COUNT() > 0) {
		evt = QUEUE_AT(QUEUE_COUNT() - 1);
		if (evt->type == W_GUI_EVENT_MOVE && evt->source == source) {
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
	evt->source = source;
	evt->type   = type;
	evt->x      = x;
	evt->y      = y;
	evt->value  = value;
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
		if (evt->source == source) continue;
		if (kept != n) *QUEUE_AT(kept) = *evt;
		kept++;
	}
	Event_Head = Event_Tail + kept;
}


// Undoes what keeps a handle context alive while its native object exists.
static void Release_Handle(REBHOB *hob)
{
	if (!hob) return;
	Purge_Events(hob);
	hob->flags &= ~HANDLE_CONTEXT_LOCKED;
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

	if (widget->owner) {
		GUIWIDGET **link = (GUIWIDGET**)&widget->owner->widgets;
		while (*link) {
			if (*link == widget) { *link = (GUIWIDGET*)widget->next; break; }
			link = (GUIWIDGET**)&(*link)->next;
		}
		widget->owner = NULL;
	}
	widget->handle = NULL;
	widget->next   = NULL;
	Release_Handle(widget->hob);
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

// ... and as a live widget handle.
static GUIWIDGET* Frm_Widget(RXIFRM *frm, REBCNT n)
{
	REBHOB *hob;
	if (!FRM_IS_HANDLE(n, Handle_GuiWidget)) return NULL;
	hob = RXA_HANDLE_CONTEXT(frm, n);
	if (!hob || !IS_USED_HOB(hob) || !hob->data) return NULL;
	return (GUIWIDGET*)hob->data;
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

	if (!wid || !wid->hob || !wid->hob->series) return FALSE;
	img = wid->hob->series;

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

	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	if (RXA_REF(frm, 2)) { // /title
		int len = RL_GET_UTF8_STRING(RXA_SERIES(frm, 3), RXA_INDEX(frm, 3), (void**)&title);
		if (len > 0) title_len = (REBCNT)len;
	}
	if (RXA_REF(frm, 4)) { // /at
		x = (REBINT)RXA_PAIR(frm, 5).x;
		y = (REBINT)RXA_PAIR(frm, 5).y;
	}

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWindow);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	win = (GUIWIN*)hob->data;
	win->hob = hob; // the window procedure tags its events with it

	if (!Gui_Open_Window(win, x, y, w, h, title, title_len)) {
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WINDOW);
	}

	// The native window and every queued event point back at this context,
	// so the GC must leave it alone until the window is closed.
	hob->flags |= HANDLE_CONTEXT_LOCKED;

	if (!RXA_REF(frm, 6)) Gui_Show_Window(win, TRUE); // /hidden

	RETURN_HANDLE(hob);
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
	if (win->handle) Gui_Close_Window(win); // -> Gui_Window_Closed()

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
**  poll-events
**
**  Dispatches everything the OS has waiting - which is what fills the
**  queue - and returns the collected events as one flat block of
**  four-value records:
**
**      foreach [type window position value] poll-events [...]
**
**  Always returns a block, empty when nothing happened, so the caller
**  never has to test before iterating.
***********************************************************************/
COMMAND cmd_gui_poll_events(RXIFRM *frm, void *ctx)
{
	REBSER *blk;
	REBCNT  count, n;
	RXIARG  val;

	Gui_Pump();

	if (Event_Dropped) {
		printf("GUI: dropped %u events (queue full)\n", Event_Dropped);
		Event_Dropped = 0;
	}

	count = QUEUE_COUNT();
	blk = (REBSER*)RL_MAKE_BLOCK(count * 4);
	if (!blk) RETURN_ERROR(ERR_NO_HANDLE);

	// RL_Set_Value may expand the block, and an expansion can collect - so
	// the series is protected until it is safely stored in the frame.
	RL_PROTECT_GC(blk, 1);

	for (n = 0; n < count; n++) {
		GUIEVT *evt = QUEUE_AT(n);
		REBCNT  i = n * 4;

		// 1. event type, as a word: `move`, `down`, `wheel`, ...
		CLEARS(&val);
		val.int32a = (i32)Gui_event_words[evt->type];
		RL_SET_VALUE(blk, i, val, RXT_WORD);

		// 2. what produced it - a window, or a widget for `click`
		Set_Handle_Arg(&val, evt->source);
		RL_SET_VALUE(blk, i + 1, val, RXT_HANDLE);

		// 3. position in client coordinates (the new size for `resize`)
		CLEARS(&val);
		val.pair.x = (float)evt->x;
		val.pair.y = (float)evt->y;
		RL_SET_VALUE(blk, i + 2, val, RXT_PAIR);

		// 4. modifier bits, or the wheel delta in lines
		CLEARS(&val);
		val.int64 = (i64)evt->value;
		RL_SET_VALUE(blk, i + 3, val, RXT_INTEGER);
	}

	RL_PROTECT_GC(blk, 0);
	Event_Tail += count;

	RXA_SERIES(frm, 1) = blk;
	RXA_INDEX(frm, 1) = 0;
	RXA_TYPE(frm, 1) = RXT_BLOCK;
	return RXR_VALUE;
}


/***********************************************************************
**  add-button window [handle!] text [string!] offset [pair!] size [pair!]
**
**  The widget handle is independent of the window handle: it can be kept,
**  dropped or released on its own, and it survives the window's death as
**  a closed widget rather than as a dangling pointer.
***********************************************************************/
COMMAND cmd_gui_add_button(RXIFRM *frm, void *ctx)
{
	REBHOB    *hob;
	GUIWIDGET *wid;
	GUIWIN    *win = Frm_Window(frm, 1);
	REBYTE    *text = NULL;
	REBCNT     text_len = 0;
	REBINT     x, y, w, h;
	int        len;

	if (!win || !win->handle) RETURN_ERROR(ERR_INVALID_HANDLE);

	len = RL_GET_UTF8_STRING(RXA_SERIES(frm, 2), RXA_INDEX(frm, 2), (void**)&text);
	if (len > 0) text_len = (REBCNT)len;

	x = (REBINT)RXA_PAIR(frm, 3).x;
	y = (REBINT)RXA_PAIR(frm, 3).y;
	w = (REBINT)RXA_PAIR(frm, 4).x;
	h = (REBINT)RXA_PAIR(frm, 4).y;
	if (w <= 0 || h <= 0) RETURN_ERROR(ERR_BAD_SIZE);

	hob = RL_MAKE_HANDLE_CONTEXT(Handle_GuiWidget);
	if (hob == NULL) RETURN_ERROR(ERR_NO_HANDLE);

	wid = (GUIWIDGET*)hob->data;
	wid->hob   = hob;
	wid->kind  = W_GUI_WIDGET_BUTTON;
	wid->owner = win;

	if (!Gui_Create_Button(wid, win, x, y, w, h, text, text_len)) {
		wid->owner = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	// Linked here rather than by the backend, so that the list has exactly
	// one owner and both platforms behave the same.
	wid->next = win->widgets;
	win->widgets = wid;

	hob->flags |= HANDLE_CONTEXT_LOCKED;

	RETURN_HANDLE(hob);
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
	GUIWIN    *win = Frm_Window(frm, 1);
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
	wid->hob   = hob;
	wid->kind  = W_GUI_WIDGET_IMAGE;
	wid->owner = win;

	// The GC marks a handle context's series - which is exactly what keeps
	// the image alive for as long as a widget is showing it.
	hob->series = img;

	if (!Gui_Create_Image(wid, win, x, y, w, h)) {
		wid->owner  = NULL;
		hob->series = NULL;
		RL_FREE_HANDLE_CONTEXT(hob);
		RETURN_ERROR(ERR_NO_WIDGET);
	}

	wid->next = win->widgets;
	win->widgets = wid;

	hob->flags |= HANDLE_CONTEXT_LOCKED;

	RETURN_HANDLE(hob);
}


COMMAND cmd_gui_remove_widget(RXIFRM *frm, void *ctx)
{
	GUIWIDGET *wid = Frm_Widget(frm, 1);

	if (!wid) RETURN_ERROR(ERR_INVALID_HANDLE);

	if (wid->handle) {
		Gui_Destroy_Widget(wid); // the native control
		Gui_Widget_Closed(wid);  // the Rebol side of it
	}
	return RXR_TRUE;
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
	// it had queued.
	if (win->handle) Gui_Close_Window(win);

	debug_print("releasing GUI window handle: %p\n", (void*)win);
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
	if (!win->handle && word != W_GUI_ARG_OPENQ && word != W_GUI_ARG_ID) {
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

	case W_GUI_ARG_ID:
		*type = RXT_INTEGER;
		arg->int64 = (i64)(REBUPT)win->handle;
		break;

	case W_GUI_ARG_OPENQ:
		*type = RXT_LOGIC;
		arg->int32a = (win->handle != NULL);
		break;

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
		Gui_Destroy_Widget(wid);
		Gui_Widget_Closed(wid);
	}
	debug_print("releasing GUI widget handle: %p\n", (void*)wid);
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
	// window does - except for `id`, which reports the null it now holds.
	if (!wid->handle && word != W_GUI_ARG_ID) {
		*type = RXT_NONE;
		return PE_USE;
	}

	switch (word) {
	// Accessors which only make sense for one kind answer with none on the
	// others, rather than erroring - the same as a closed widget does.
	case W_GUI_ARG_TEXT: {
		REBSER *str;
		if (wid->kind != W_GUI_WIDGET_BUTTON) { *type = RXT_NONE; break; }
		str = Gui_Widget_Get_Text(wid);
		if (!str) { *type = RXT_NONE; break; }
		arg->series = str;
		arg->index  = 0;
		*type = RXT_STRING;
		break; }

	case W_GUI_ARG_IMAGE:
		if (wid->kind != W_GUI_WIDGET_IMAGE || !hob->series) {
			*type = RXT_NONE;
			break;
		}
		arg->image  = hob->series;
		arg->width  = (int)IMG_WIDE(hob->series);
		arg->height = (int)IMG_HIGH(hob->series);
		*type = RXT_IMAGE;
		break;

	case W_GUI_ARG_KIND:
		// The word list the kind was taken from is also how it is named.
		*type = RXT_WORD;
		arg->int32a = (i32)Gui_widget_words[wid->kind];
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

	case W_GUI_ARG_ID:
		*type = RXT_INTEGER;
		arg->int64 = (i64)(REBUPT)wid->handle;
		break;

	case W_GUI_ARG_ENABLEDQ:
		if (wid->kind != W_GUI_WIDGET_BUTTON) { *type = RXT_NONE; break; }
		*type = RXT_LOGIC;
		arg->int32a = Gui_Widget_Get_Enabled(wid);
		break;

	case W_GUI_ARG_PARENT:
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

	switch (RL_FIND_WORD(Gui_arg_words, word)) {
	case W_GUI_ARG_TEXT: {
		REBYTE *utf8 = NULL;
		int len;
		if (wid->kind != W_GUI_WIDGET_BUTTON) return PE_BAD_SET;
		if (*type != RXT_STRING) return PE_BAD_SET_TYPE;
		len = RL_GET_UTF8_STRING((REBSER*)arg->series, arg->index, (void**)&utf8);
		if (len < 0) return PE_BAD_SET;
		Gui_Widget_Set_Text(wid, utf8, (REBCNT)len);
		break; }

	// Swapping the image is just swapping the reference the GC marks; the
	// widget keeps its box and the new image is scaled into it.
	case W_GUI_ARG_IMAGE:
		if (wid->kind != W_GUI_WIDGET_IMAGE) return PE_BAD_SET;
		if (*type != RXT_IMAGE) return PE_BAD_SET_TYPE;
		if (!arg->image) return PE_BAD_SET;
		hob->series = (REBSER*)arg->image;
		Gui_Widget_Redraw(wid);
		break;

	// Both halves of the box are read back first, so that setting one does
	// not disturb the other.
	case W_GUI_ARG_SIZE:
		if (*type != RXT_PAIR) return PE_BAD_SET_TYPE;
		if (arg->pair.x <= 0 || arg->pair.y <= 0) return PE_BAD_RANGE;
		if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) return PE_BAD_SET;
		Gui_Widget_Set_Box(wid, x, y, (REBINT)arg->pair.x, (REBINT)arg->pair.y);
		break;

	case W_GUI_ARG_OFFSET:
		if (*type != RXT_PAIR) return PE_BAD_SET_TYPE;
		if (!Gui_Widget_Get_Box(wid, &x, &y, &w, &h)) return PE_BAD_SET;
		Gui_Widget_Set_Box(wid, (REBINT)arg->pair.x, (REBINT)arg->pair.y, w, h);
		break;

	case W_GUI_ARG_ENABLEDQ:
		if (wid->kind != W_GUI_WIDGET_BUTTON) return PE_BAD_SET;
		if (*type != RXT_LOGIC) return PE_BAD_SET_TYPE;
		Gui_Widget_Set_Enabled(wid, arg->int32a ? TRUE : FALSE);
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
			(wid->kind == W_GUI_WIDGET_IMAGE) ? "image" : "button");
	} else {
		APPEND_STRING(str, "%s", "removed");
	}
	return len;
}
