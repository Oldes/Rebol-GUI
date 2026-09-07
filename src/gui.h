//
// Project: Rebol/GUI extension
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
// Shared between the entry points (gui.c), the command sources
// (gui-commands.c) and the platform backend (gui-win.c).
//
// The GUIWIN struct and the `Handle_GuiWindow` extern come from the
// generated header, via the specification's `c-header:` field.
//

#ifndef GUI_EXT_H
#define GUI_EXT_H

//== window handle callbacks ==================================================
// Named after the HANDLE TYPE, not after the extension - the extension owns
// Gui_Init() and the generated Gui_* symbols. Registered by Gui_Init();
// implemented in gui-commands.c.

int GuiWindow_free(void *hndl);
int GuiWindow_get_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg);
int GuiWindow_set_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg);
int GuiWindow_mold(REBHOB *hob, REBSER *str);

int GuiWidget_free(void *hndl);
int GuiWidget_get_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg);
int GuiWidget_set_path(REBHOB *hob, REBCNT word, REBCNT *type, RXIARG *arg);
int GuiWidget_mold(REBHOB *hob, REBSER *str);


//== event queue ==============================================================
// Filled by the platform's window procedure, drained by `poll-events`.
// Implemented in gui-commands.c so that the consumer owns the buffer.

typedef struct Gui_Event {
	REBHOB *source; // handle context which produced it: a window, or a widget
	REBCNT  type;   // W_GUI_EVENT_* - also the index into Gui_event_words
	REBINT  x, y;   // position in client coordinates
	REBINT  value;  // modifier bits, or the wheel delta in lines
} GUIEVT;

// Modifier bits reported in GUIEVT.value; mirrored by `event-flags` in the
// module's mezzanine, so both sides must be changed together.
enum {
	GUI_FLAG_SHIFT   = 1,
	GUI_FLAG_CONTROL = 2,
	GUI_FLAG_ALT     = 4,
	GUI_FLAG_DOUBLE  = 8
};

// Never blocks and never allocates - a full queue drops the event and bumps
// a counter, which `poll-events` reports once instead of failing silently.
void   Gui_Queue_Event(REBHOB *source, REBCNT type, REBINT x, REBINT y, REBINT value);

// Called by the backend once a native window has really gone away, however
// that happened: drops the queued events which point at the handle context
// and releases the GC lock that an open window holds on it.
//
// It does the same for every widget still on the window's list, because the
// OS destroys child controls together with their parent - so a backend must
// have finished releasing its own native child objects before calling this.
void   Gui_Window_Closed(REBHOB *window);

// The single-widget counterpart: unlinks it from its window, drops its
// queued events and unlocks its handle. Does NOT touch the native control.
void   Gui_Widget_Closed(GUIWIDGET *widget);

// What a backend calls when a control was activated, instead of queueing
// the `click` itself: check and radio state is settled here first, because
// radio groups are this extension's business rather than the platform's.
void   Gui_Widget_Activated(GUIWIDGET *widget, REBINT x, REBINT y, REBINT flags);


//== platform backend =========================================================
// Everything below is implemented per platform (currently gui-win.c only).
// None of it takes a REBGOB or touches the host's View sources.

// Passed as x/y to Gui_Open_Window to let the system place the window.
// Chosen to match Win32's CW_USEDEFAULT.
#define GUI_DEFAULT_POS ((REBINT)0x80000000)

void    Gui_Init_Platform(void);
void    Gui_Quit_Platform(void);

// `w`/`h` are the CLIENT size; the frame is added on top of it. `title` is
// UTF-8 and does not need to be null terminated.
REBOOL  Gui_Open_Window(GUIWIN *win, REBINT x, REBINT y, REBINT w, REBINT h,
                        const REBYTE *title, REBCNT title_len,
                        REBCNT flags); // GUI_WIN_* bits
void    Gui_Close_Window(GUIWIN *win);
void    Gui_Show_Window(GUIWIN *win, REBOOL show);

// Dispatches everything waiting in the OS queue, which is what turns
// messages into Gui_Queue_Event() calls.
void    Gui_Pump(void);

REBOOL  Gui_Get_Size(GUIWIN *win, REBINT *w, REBINT *h);
REBOOL  Gui_Get_Offset(GUIWIN *win, REBINT *x, REBINT *y);
REBOOL  Gui_Set_Size(GUIWIN *win, REBINT w, REBINT h);
REBOOL  Gui_Set_Offset(GUIWIN *win, REBINT x, REBINT y);

/***********************************************************************
**  COORDINATES ARE LOGICAL UNITS, EVERYWHERE ACROSS THIS INTERFACE.
**
**  96 to the inch - which is what macOS calls a point, and what a
**  Windows program means by a pixel at 100% scaling. So `240x26` is the
**  same physical size on a 96 DPI screen, on a 175% one, and on a
**  Retina Mac, and one layout is right on all of them.
**
**  A backend converts at its own boundary: the Cocoa one has nothing to
**  do, because AppKit already works in points; the Win32 one multiplies
**  by the system DPI on the way in and divides on the way out, INCLUDING
**  the coordinates it puts into events. Nothing above gui.h knows the
**  difference.
**
**  Pixels appear in exactly one place: an image widget's own pixels,
**  which are stretched into whatever box it was given. Gui_Get_Scale()
**  is how a caller sizes an image to land on device pixels one for one.
***********************************************************************/
REBDEC  Gui_Get_Scale(GUIWIN *win);

// What a widget needs for the text it holds, in logical units - border,
// padding and all. FALSE for a kind with no text, which has no natural
// size to give. Called after the widget's font is settled, because the
// answer depends on it.
REBOOL  Gui_Widget_Natural_Size(GUIWIDGET *wid, REBINT *w, REBINT *h);

/***********************************************************************
**  The window's frame.
**
**  Both are read from the native window rather than shadowed, for the
**  same reason a widget's font is: what is reported is then what is on
**  screen, whatever else may have changed it.
**
**  Changing either changes how much of the window the frame takes, so
**  both setters keep the CLIENT size the caller asked for - the same
**  promise Gui_Menu_End() makes.
**
**  A window with no border has no title bar, which means no close box
**  and nothing to drag: `close` events stop arriving and the program is
**  the only thing that can move it (`offset`) or close it.
***********************************************************************/
REBOOL  Gui_Get_Resizable(GUIWIN *win);
REBOOL  Gui_Set_Resizable(GUIWIN *win, REBOOL on);
REBOOL  Gui_Get_Border(GUIWIN *win);
REBOOL  Gui_Set_Border(GUIWIN *win, REBOOL on);

// Allocates a Rebol string series; returns NULL on failure.
REBSER* Gui_Get_Title(GUIWIN *win);
REBOOL  Gui_Set_Title(GUIWIN *win, const REBYTE *utf8, REBCNT len);


/***********************************************************************
**  The menu bar.
**
**  Built rather than handed over: the shared layer walks the block the
**  caller wrote and calls these in order, so the dialect is parsed once,
**  in one place, and a backend never sees a Rebol value.
**
**      Gui_Menu_Begin(win)
**        Gui_Menu_Add_Popup(win, NULL, "File")        -> handle
**          Gui_Menu_Add_Item(win, file, "New", 1, ...)
**          Gui_Menu_Add_Separator(win, file)
**      Gui_Menu_End(win)
**
**  A NULL parent means the bar itself. The `parent` values are whatever
**  the backend returned from Gui_Menu_Add_Popup - the shared layer only
**  passes them back.
**
**  Item ids are 1-based indices into win->menu_ids, small enough for
**  Win32, which carries a menu id in the low 16 bits of a WM_COMMAND.
**  A backend reports a pick by calling Gui_Menu_Picked() below.
***********************************************************************/
/***********************************************************************
**  Sleeps until the OS has something to deliver, or the timeout runs
**  out - whichever comes first. Returns at once when something is
**  already waiting.
**
**  This is what lets an event loop be responsive without spinning. It
**  matters more than it looks: a themed control animates on TIMER
**  messages, and a loop which sleeps a fixed interval and then drains
**  serves those timers late and in bursts, which is what makes a modern
**  Windows theme feel sluggish where the classic one - which does not
**  animate at all - feels fine. Waking on the message itself hands the
**  animation its timer the moment it is due.
**
**  It does NOT dispatch anything: Gui_Pump() still does that.
***********************************************************************/
void    Gui_Wait(REBINT ms);

REBOOL  Gui_Menu_Begin(GUIWIN *win);
void*   Gui_Menu_Add_Popup(GUIWIN *win, void *parent,
                           const REBYTE *label, REBCNT len);
void    Gui_Menu_Add_Item(GUIWIN *win, void *parent,
                          const REBYTE *label, REBCNT len, REBCNT id,
                          // `key` is 0 for no shortcut, otherwise the
                          // character; `mods` is GUI_FLAG_* bits ON TOP of
                          // the platform's own menu modifier - Ctrl on
                          // Windows, Command on macOS.
                          REBCNT key, REBCNT mods);
void    Gui_Menu_Add_Separator(GUIWIN *win, void *parent);
REBOOL  Gui_Menu_End(GUIWIN *win);

// Takes the bar off the window and destroys everything Gui_Menu_Begin()
// made. Safe to call when there is no menu, and on a closed window.
void    Gui_Menu_Free(GUIWIN *win);

// Greys one item out, or brings it back. The shared layer keeps the flag;
// this only applies it.
void    Gui_Menu_Enable(GUIWIN *win, REBCNT id, REBOOL enabled);

// Called BY a backend when an item is picked - it queues the `menu` event
// with the item's word, which is the shared layer's business.
void    Gui_Menu_Picked(GUIWIN *win, REBCNT id);


//-- widgets ------------------------------------------------------------------
// `x`/`y` are relative to the window's client area, top-left origin.
//
// The backend fills in `wid->handle` only; linking the widget onto its
// window's list is done by the caller, so that the list stays in one place.

// The push button and the two toggles: one BUTTON class on Windows, one
// NSButton on macOS, differing only in style bits and button type, which
// `wid->kind` - already set by the caller - selects.
REBOOL  Gui_Create_Button_Control(GUIWIDGET *wid, GUIWIN *owner,
                                  REBINT x, REBINT y, REBINT w, REBINT h,
                                  const REBYTE *text, REBCNT len);

// On/off state of a check or a radio, as the native control holds it.
REBOOL  Gui_Widget_Get_State(GUIWIDGET *wid);
void    Gui_Widget_Set_State(GUIWIDGET *wid, REBOOL on);

// The slider and the progress bar. Neither carries a label, so no text is
// passed in; `wid->kind` picks between them, and a box taller than it is
// wide makes a slider vertical.
REBOOL  Gui_Create_Range_Control(GUIWIDGET *wid, GUIWIN *owner,
                                 REBINT x, REBINT y, REBINT w, REBINT h);

// Position as a fraction from 0.0 to 1.0. Both backends normalise it so
// that a vertical slider reads 0.0 at the BOTTOM, whatever the platform's
// own idea of which end is the origin.
REBDEC  Gui_Widget_Get_Value(GUIWIDGET *wid);
void    Gui_Widget_Set_Value(GUIWIDGET *wid, REBDEC value);


//-- drop-down ----------------------------------------------------------------
// Deliberately one item at a time. Turning a Rebol block into a list, and a
// list back into a block, is the same work on every platform and is done
// once in gui-commands.c - a backend only has to know how to hold strings.

REBOOL  Gui_Create_Drop_Down(GUIWIDGET *wid, GUIWIN *owner,
                             REBINT x, REBINT y, REBINT w, REBINT h);


//-- panel --------------------------------------------------------------------
// A container. Everything a panel holds is positioned inside IT rather than
// inside the window, which is what both platforms do natively for a child.
//
// NOTE for backends: `wid->parent` is set before any Gui_Create_* call, so
// the native parent to attach to is the panel's when it is set and the
// window's otherwise. Every creation function reads it that way.
//
// `wid->state & GUI_PANEL_EDGE` - set by the caller before this is called,
// and changeable afterwards through the `edge` accessor - says whether the
// panel draws a frame around itself, with `text` as its caption. Both are
// read at PAINT time, never cached, which is what makes the accessor work
// without recreating the control.
//
// The frame is drawn INSIDE the panel's own box and does not move anything:
// a child is still positioned from the panel's top-left corner, framed or
// not, so turning an edge on never shifts what the panel holds. Room for
// the frame and the caption is the caller's to leave. The metrics come from
// each platform's own font, so the two do not agree to the pixel - which is
// exactly why they are not allowed to affect layout.
REBOOL  Gui_Create_Panel(GUIWIDGET *wid, GUIWIN *owner,
                         REBINT x, REBINT y, REBINT w, REBINT h,
                         const REBYTE *text, REBCNT len);

// Repaints a panel after its edge or caption changed. A backend which draws
// the frame in its own paint handler only has to invalidate.
void    Gui_Panel_Edge_Changed(GUIWIDGET *wid);

REBCNT  Gui_Widget_Count_Items(GUIWIDGET *wid);
REBSER* Gui_Widget_Get_Item(GUIWIDGET *wid, REBCNT n);   // 0-based
REBOOL  Gui_Widget_Add_Item(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len);
void    Gui_Widget_Clear_Items(GUIWIDGET *wid);

// 0-based, and -1 for "nothing picked" - the 1-based Rebol index is the
// shared layer's business, not a backend's.
REBINT  Gui_Widget_Get_Index(GUIWIDGET *wid);
void    Gui_Widget_Set_Index(GUIWIDGET *wid, REBINT n);

// No pixels are passed in: the image lives in the handle's series and is
// read again at every paint, so that drawing into it is all it takes to
// change what is on screen.
REBOOL  Gui_Create_Image(GUIWIDGET *wid, GUIWIN *owner,
                         REBINT x, REBINT y, REBINT w, REBINT h);

// The label and the two text entries share one entry point: they differ
// only in style flags on Windows, and `wid->kind` - already set by the
// caller - is what picks between them on both platforms.
REBOOL  Gui_Create_Text_Control(GUIWIDGET *wid, GUIWIN *owner,
                                REBINT x, REBINT y, REBINT w, REBINT h,
                                const REBYTE *text, REBCNT len);

// What a backend calls while painting an image widget. Returns FALSE when
// the widget has no image yet, or the image is empty.
//
// The data pointer is only valid for the duration of the paint - a series
// can move - so it is fetched here rather than cached anywhere.
REBOOL  Gui_Widget_Pixels(GUIWIDGET *wid, REBYTE **data, REBINT *w, REBINT *h);

// Marks the whole thing as needing a repaint. Both are safe on a closed
// window or a removed widget, where they do nothing.
void    Gui_Widget_Redraw(GUIWIDGET *wid);
void    Gui_Window_Redraw(GUIWIN *win);

// Destroys the native control. Safe on a widget whose window is already
// gone - it then does nothing, because the OS took the control with it.
void    Gui_Destroy_Widget(GUIWIDGET *wid);

REBSER* Gui_Widget_Get_Text(GUIWIDGET *wid);
REBOOL  Gui_Widget_Set_Text(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len);

/***********************************************************************
**  Typography.
**
**  One getter and one setter for the whole font, rather than a pair per
**  property: a native font is one indivisible object on both platforms,
**  so changing only the size means reading the font, changing the size
**  and putting it back. The shared layer does exactly that, which is
**  why `font`, `font-size`, `bold?` and `italic?` are four accessors
**  above but two functions here.
**
**  The font is NOT shadowed in the widget context - it is read from the
**  control every time, so what Rebol reports is what the control has.
**
**  `name` is a fresh Rebol string, the caller's to keep, and NULL when
**  the control has no font of its own. `size` is in points, 0 when
**  unknown. `style` is GUI_FONT_* bits. Any of the three may be NULL.
***********************************************************************/
REBOOL  Gui_Widget_Get_Font(GUIWIDGET *wid, REBSER **name, REBINT *size,
                            REBCNT *style);

// A NULL name asks for the platform's own font family, and a size of 0
// for its own size, which is how `font: none` and `font-size: none` are
// carried through.
REBOOL  Gui_Widget_Set_Font(GUIWIDGET *wid, const REBYTE *name, REBCNT len,
                            REBINT size, REBCNT style);

// Applies `wid->color`, which the caller has already set - the colour is
// shared-layer state, so there is nothing to pass and nothing to read
// back. A backend which cannot colour a particular control (a Win32 push
// button is the one case) returns FALSE, and the accessor still reports
// the colour that was asked for.
REBOOL  Gui_Widget_Set_Color(GUIWIDGET *wid);

// Position and size travel together: both accessors read the whole box and
// write back the half they changed.
REBOOL  Gui_Widget_Get_Box(GUIWIDGET *wid, REBINT *x, REBINT *y, REBINT *w, REBINT *h);
REBOOL  Gui_Widget_Set_Box(GUIWIDGET *wid, REBINT x, REBINT y, REBINT w, REBINT h);

REBOOL  Gui_Widget_Get_Enabled(GUIWIDGET *wid);
REBOOL  Gui_Widget_Set_Enabled(GUIWIDGET *wid, REBOOL enabled);


// Gui_Init() is declared in gen-gui.h - the generated `_init` handler calls
// it, and every extension is required to define one.

#ifndef REB_EXT
// Embedded build only - called from host-main.c under INCLUDE_EXT_GUI.
RL_API void OS_Init_Ext_Gui(void);
#endif

#endif // GUI_EXT_H
