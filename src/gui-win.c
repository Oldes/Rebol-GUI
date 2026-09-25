//
// Project: Rebol/GUI extension
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
// Win32 backend.
//
// The only file which knows about HWNDs. Its whole job is to turn window
// messages into Gui_Queue_Event() calls - it never calls back into the
// interpreter except to build a title string, so nothing here can run
// Rebol code from inside a modal OS loop.
//
// The W (wide) entry points are used explicitly rather than the TCHAR
// macros, so the build does not depend on UNICODE being defined. Rebol
// strings are UTF-8 and are converted at the boundary.
//

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM
#include <commctrl.h> // trackbar and progress bar
#include <shellapi.h> // DragQueryFile and friends
#define  COBJMACROS   // so the IDropTarget vtable can be used from plain C
#include <ole2.h>     // OLE drag and drop
// MAKE_MEM / FREE_MEM are malloc and free behind a macro, and neither
// rebol-extension.h nor windows.h is required to declare them - the wide
// string conversions and the font cache here both allocate.
#include <stdlib.h>
#include <string.h> // strcmp
#include <wchar.h>  // wcslen
// The drag and drop trace prints; MSVC does not get stdio from windows.h.
#include <stdio.h>

// Windows uses this macro name too, and we want Rebol's meaning of it.
#undef IS_ERROR

#include "gen-gui.h"
#include "gui.h"

//***** Locals *****//

static const WCHAR *Class_Name = L"RebolGuiWindow";
static const WCHAR *Class_Name_Image = L"RebolGuiImage";
static const WCHAR *Class_Name_Panel = L"RebolGuiPanel";
static HINSTANCE App_Instance = NULL;
static REBOOL Class_Registered = FALSE;
static REBOOL Image_Class_Registered = FALSE;
static REBOOL Panel_Class_Registered = FALSE;
static HFONT  Default_Font = NULL;
static REBOOL Default_Font_Owned = FALSE;

// Raised around SetWindowTextW so that writing to an edit control from Rebol
// does not come back as a `change` event. SetWindowText delivers EN_CHANGE
// synchronously on this same thread, so a plain flag is enough.
static REBOOL Setting_Text = FALSE;

// WS_CLIPCHILDREN, for the same reason a panel has it: without it a
// parent's WM_PAINT paints straight over the controls it holds. The DC
// BeginPaint hands back covers the whole invalid region, children
// included, so one FillRect erases every control it crosses - and only
// the parts which then get a WM_PAINT of their own come back.
//
// What does NOT come back is anything in a control's NON-client area. An
// entry's sunken border is drawn on WM_NCPAINT, which invalidating the
// client area never raises, so the border stays missing until something
// else disturbs it. That is what a field with its top edge rubbed out
// after a neighbour was resized over it really was.
//
// With the flag, the parent is clipped out of every child rectangle and
// physically cannot do it.
#define WINDOW_STYLE   (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN)
#define WINDOW_EXSTYLE (0)

// What a window's frame is made of, as bits of the window style.
//
//   WS_THICKFRAME is the grab handle; WS_MAXIMIZEBOX goes with it, since
//   a window which cannot be dragged bigger should not have a button
//   which does it either.
//   WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX are the title bar and what
//   sits on it. Without them there is no close box, so no `close` event,
//   and nothing to drag the window by.
#define WINDOW_RESIZE_BITS (WS_THICKFRAME | WS_MAXIMIZEBOX)
#define WINDOW_BORDER_BITS (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

// Trackbars and progress bars work in whole steps, so the 0.0 - 1.0 range
// the extension speaks is carried as one part in RANGE_STEPS.
#define RANGE_STEPS 1000

// A combo box is created with the height of the WHOLE thing - the closed
// control plus the list it drops down - so room for the list has to be
// added to whatever height the caller asked for. Ask for too little and
// the list is a sliver; this is the classic Win32 trap with this control.
#define DROP_LIST_ROOM 220

#define HWND_OF(win)      ((HWND)((win)->handle))
#define HWND_OF_WID(wid)  ((HWND)((wid)->handle))
#define GUIWIN_OF(hwnd)   ((GUIWIN*)GetWindowLongPtrW((hwnd), GWLP_USERDATA))

// Defined with the drop target, far below, but needed by WM_NCDESTROY: a
// registered target must go while its HWND is still valid.
static void Gui_Window_Revoke_Drop(GUIWIN *win);

// Defined with the keyboard handling, below; installed by every control
// creation, which comes first in the file.
static void   Subclass_For_Nav(GUIWIDGET *wid);
static REBOOL Gui_Handle_Key(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Whether OLE came up on this thread, decided once in Gui_Init_Platform,
// and whether it was us who started it - see the note there.
static REBOOL Ole_Ready = FALSE;
static REBOOL Ole_Ours  = FALSE;


//== string conversion ========================================================

// UTF-8 (not necessarily terminated) -> a fresh, null terminated UTF-16
// buffer. Release it with FREE_MEM.
static WCHAR* To_Wide(const REBYTE *utf8, REBCNT len)
{
	int n;
	WCHAR *out;

	if (!utf8 || len == 0) return NULL;

	n = MultiByteToWideChar(CP_UTF8, 0, (const char*)utf8, (int)len, NULL, 0);
	if (n <= 0) return NULL;

	out = (WCHAR*)MAKE_MEM((n + 1) * sizeof(WCHAR));
	if (!out) return NULL;

	MultiByteToWideChar(CP_UTF8, 0, (const char*)utf8, (int)len, out, n);
	out[n] = 0;
	return out;
}

// Window text -> a fresh Rebol string series. Shared by the window title and
// the widget label, which are the same Win32 call underneath.
static REBSER* Text_Of(HWND hwnd)
{
	int    len;
	WCHAR *buf;
	REBSER *str;

	if (!hwnd) return NULL;

	len = GetWindowTextLengthW(hwnd);
	if (len <= 0) return RL_MAKE_STRING(0, FALSE);

	buf = (WCHAR*)MAKE_MEM((len + 1) * sizeof(WCHAR));
	if (!buf) return NULL;

	len = GetWindowTextW(hwnd, buf, len + 1);

	// REBUNI is 16 bits, so the wide buffer is passed through as is.
	str = RL_ENCODE_UTF8_STRING(buf, (REBCNT)len, TRUE, 0);
	FREE_MEM(buf);
	return str;
}

static REBOOL Set_Text_Of(HWND hwnd, const REBYTE *utf8, REBCNT len)
{
	WCHAR *wide;
	BOOL ok;

	if (!hwnd) return FALSE;

	wide = To_Wide(utf8, len);
	Setting_Text = TRUE;
	ok = SetWindowTextW(hwnd, wide ? wide : L"");
	Setting_Text = FALSE;
	if (wide) FREE_MEM(wide);
	return ok ? TRUE : FALSE;
}

// The font controls are created with by default is the ancient system font,
// so the shell's message font is used instead - the one every other dialog
// on the machine uses.
static HFONT Get_Default_Font(void)
{
	NONCLIENTMETRICSW metrics;

	if (Default_Font) return Default_Font;

	ZeroMemory(&metrics, sizeof(metrics));
	metrics.cbSize = sizeof(metrics);
	if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
		Default_Font = CreateFontIndirectW(&metrics.lfMessageFont);
		Default_Font_Owned = (Default_Font != NULL);
	}
	if (!Default_Font) Default_Font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	return Default_Font;
}


/***********************************************************************
**  Fonts asked for from Rebol.
**
**  An HFONT is a GDI object which has to be deleted by whoever made it,
**  and a control does not own the one it is given. Rather than hang one
**  off every widget - and get it wrong at teardown exactly once - the
**  fonts are kept in a small cache, shared by every control that asks
**  for the same face, and deleted together in Gui_Quit_Platform().
**
**  A program restyling one label in a loop therefore creates one font,
**  not one per assignment.
***********************************************************************/
typedef struct Gui_Font_Cache {
	struct Gui_Font_Cache *next;
	HFONT  font;
	int    size;    // points
	REBCNT style;   // GUI_FONT_* bits
	int    dpi;     // what it was made for
	WCHAR  name[LF_FACESIZE];
} FONTCACHE;

static FONTCACHE *Font_Cache = NULL;

/***********************************************************************
**  Logical units, and which DPI they are converted at.
**
**  Everything above this file speaks LOGICAL units - 96 to the inch,
**  the same thing macOS calls a point - and this is where they become
**  device pixels and back. `240x26` therefore describes the same
**  physical size on a 96 DPI screen, on a 175% one, and on a Mac.
**
**  The process is PER-MONITOR DPI aware where Windows can do it (10,
**  version 1607 and later), so there is no single DPI any more: each
**  window has the DPI of the monitor it is on, its controls share it,
**  and it changes when the window is moved to a monitor with another
**  scale setting - see WM_DPICHANGED in the window procedure. Every
**  conversion therefore names the window (or the monitor) it is for.
**
**  Where per-monitor awareness is not available, the process falls back
**  to SYSTEM awareness and every window answers the system DPI, which
**  is exactly the behaviour this file had before.
***********************************************************************/
static int    Gui_DPI = 96;         // the system DPI: the fallback everywhere
static REBOOL Per_Monitor = FALSE;  // TRUE once per-monitor awareness is on

// Late-bound: all of these are newer than the oldest Windows this runs on.
typedef UINT    (WINAPI *GETDPIFORWINDOW_T)(HWND);
typedef BOOL    (WINAPI *ADJUSTWINDOWRECTEXFORDPI_T)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef int     (WINAPI *GETSYSTEMMETRICSFORDPI_T)(int, UINT);
typedef HRESULT (WINAPI *GETDPIFORMONITOR_T)(HMONITOR, int, UINT*, UINT*);
static GETDPIFORWINDOW_T          pGetDpiForWindow          = NULL;
static ADJUSTWINDOWRECTEXFORDPI_T pAdjustWindowRectExForDpi = NULL;
static GETSYSTEMMETRICSFORDPI_T   pGetSystemMetricsForDpi   = NULL;
static GETDPIFORMONITOR_T         pGetDpiForMonitor         = NULL;

static void Read_Screen_DPI(void)
{
	HDC dc = GetDC(NULL);
	if (dc) {
		int y = GetDeviceCaps(dc, LOGPIXELSY);
		if (y > 0) Gui_DPI = y;
		ReleaseDC(NULL, dc);
	}
}

// The DPI a window - or any control in it - is drawn at right now.
static int Dpi_Of(HWND hwnd)
{
	if (Per_Monitor && hwnd) {
		UINT dpi = pGetDpiForWindow(hwnd);
		if (dpi > 0) return (int)dpi;
	}
	return Gui_DPI;
}

// The DPI a monitor is set to. MDT_EFFECTIVE_DPI (0) is the one Windows
// scales windows for, which is the one that matters here.
static int Dpi_Of_Monitor(HMONITOR mon)
{
	if (Per_Monitor && mon && pGetDpiForMonitor) {
		UINT x = 0, y = 0;
		if (SUCCEEDED(pGetDpiForMonitor(mon, 0, &x, &y)) && y > 0) return (int)y;
	}
	return Gui_DPI;
}

static REBINT To_Device(int dpi, REBINT v)
{
	return (dpi == 96) ? v : (REBINT)MulDiv((int)v, dpi, 96);
}

static REBINT To_Logical(int dpi, REBINT v)
{
	return (dpi == 96) ? v : (REBINT)MulDiv((int)v, 96, dpi);
}

// Whole boxes, which is how they nearly always travel.
static void Box_To_Device(int dpi, REBINT *x, REBINT *y, REBINT *w, REBINT *h)
{
	if (dpi == 96) return;
	if (x) *x = To_Device(dpi, *x);
	if (y) *y = To_Device(dpi, *y);
	if (w) *w = To_Device(dpi, *w);
	if (h) *h = To_Device(dpi, *h);
}

static void Box_To_Logical(int dpi, REBINT *x, REBINT *y, REBINT *w, REBINT *h)
{
	if (dpi == 96) return;
	if (x) *x = To_Logical(dpi, *x);
	if (y) *y = To_Logical(dpi, *y);
	if (w) *w = To_Logical(dpi, *w);
	if (h) *h = To_Logical(dpi, *h);
}

// A system metric at a given DPI. GetSystemMetrics() alone answers for the
// system DPI, which is wrong for a window on any other monitor.
static int Metric(int dpi, int index)
{
	if (pGetSystemMetricsForDpi) return pGetSystemMetricsForDpi(index, (UINT)dpi);
	return MulDiv(GetSystemMetrics(index), dpi, Gui_DPI);
}

// The window rectangle for a client rectangle, with the frame sized for
// the DPI the window will be at - which is not the system's on another
// monitor, and a frame computed for the wrong one is off by pixels.
static void Adjust_Rect(RECT *r, DWORD style, BOOL menu, DWORD exstyle, int dpi)
{
	if (pAdjustWindowRectExForDpi && pAdjustWindowRectExForDpi(r, style, menu, exstyle, (UINT)dpi))
		return;
	AdjustWindowRectEx(r, style, menu, exstyle);
}

// The DPI a window was last laid out at. WM_DPICHANGED arrives with
// GetDpiForWindow() already answering the NEW value, so the old one - which
// every child position and font was computed at - has to be remembered.
static const WCHAR *Dpi_Prop = L"RebolGuiDpi";

static void Remember_Dpi(HWND hwnd, int dpi)
{
	SetPropW(hwnd, Dpi_Prop, (HANDLE)(INT_PTR)dpi);
}

static int Remembered_Dpi(HWND hwnd)
{
	int dpi = (int)(INT_PTR)GetPropW(hwnd, Dpi_Prop);
	return dpi > 0 ? dpi : Gui_DPI;
}

// Screen coordinates, both ways - defined with the screens further down,
// because they are the same question as a screen's own position.
static void Screen_To_Logical(LONG px, LONG py, REBINT *x, REBINT *y);
static void Logical_To_Screen(REBINT x, REBINT y, LONG *px, LONG *py, HMONITOR *mon);
static void Keep_Client_Size(HWND hwnd, int was_w, int was_h);

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static void Free_Font_Cache(void)
{
	FONTCACHE *entry = Font_Cache;
	while (entry) {
		FONTCACHE *next = entry->next;
		if (entry->font) DeleteObject(entry->font);
		FREE_MEM(entry);
		entry = next;
	}
	Font_Cache = NULL;
}

// A NULL or empty name means the shell's message font family, and a size
// of 0 its size - which is how `font: none` and `font-size: none` arrive.
//
// Made for one DPI: the same 10 points is 13 pixels on one monitor and 20
// on another, so the cache is keyed by DPI too.
static HFONT Font_For(const WCHAR *name, int size, REBCNT style, int dpi)
{
	FONTCACHE *entry;
	LOGFONTW   base, lf;
	HFONT      font;

	// Whatever is not being asked for comes from the default font, so a
	// size on its own keeps the shell's family rather than falling back to
	// something that looks nothing like the rest of the dialog.
	ZeroMemory(&base, sizeof(base));
	if (!GetObjectW(Get_Default_Font(), sizeof(base), &base)) return NULL;

	ZeroMemory(&lf, sizeof(lf));
	lf = base;
	if (name && name[0]) {
		lstrcpynW(lf.lfFaceName, name, LF_FACESIZE);
	}
	if (size > 0) {
		lf.lfHeight = -MulDiv(size, dpi, 72);
		lf.lfWidth  = 0;
	} else if (dpi != Gui_DPI) {
		// The shell's font comes back scaled for the SYSTEM DPI.
		lf.lfHeight = MulDiv(lf.lfHeight, dpi, Gui_DPI);
		lf.lfWidth  = 0;
	}
	lf.lfWeight = (style & GUI_FONT_BOLD) ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = (style & GUI_FONT_ITALIC) ? TRUE : FALSE;

	for (entry = Font_Cache; entry; entry = entry->next) {
		if (entry->size == size && entry->style == style && entry->dpi == dpi
		    && lstrcmpW(entry->name, lf.lfFaceName) == 0)
			return entry->font;
	}

	font = CreateFontIndirectW(&lf);
	if (!font) return NULL;

	entry = (FONTCACHE*)MAKE_MEM(sizeof(FONTCACHE));
	if (!entry) { DeleteObject(font); return NULL; }
	entry->font  = font;
	entry->size  = size;
	entry->style = style;
	entry->dpi   = dpi;
	lstrcpynW(entry->name, lf.lfFaceName, LF_FACESIZE);
	entry->next  = Font_Cache;
	Font_Cache   = entry;

	return font;
}

// The shell's message font, at the DPI a control is drawn at. At the system
// DPI that is the shared default itself; anywhere else it is a scaled copy
// from the cache.
static HFONT Default_Font_At(int dpi)
{
	HFONT font;
	if (dpi == Gui_DPI) return Get_Default_Font();
	font = Font_For(NULL, 0, 0, dpi);
	return font ? font : Get_Default_Font();
}


/***********************************************************************
**  A see-through window, and what fills the client area.
**
**  WS_EX_LAYERED with a COLOUR KEY: the client area is filled with a
**  colour the compositor then drops, so child controls keep painting
**  normally and are the only thing left on screen. Per-pixel alpha
**  would mean UpdateLayeredWindow, which does not composite child
**  windows at all - it would rule out every native control this
**  extension exists to place.
**
**  The cost of a key is that a widget painting exactly this colour
**  disappears too, so it is a colour nothing sensible picks: full
**  magenta, the traditional choice for the same reason.
***********************************************************************/
#define GUI_KEY_COLOR RGB(255, 0, 255)

// What the client area of `win` is filled with. NULL when the window
// has none of its own and the system colour brush should be used, which
// FillRect takes in its encoded form and WM_CTLCOLOR* does not - hence
// two callers and two ways of asking.
static COLORREF Window_Fill_Color(GUIWIN *win, REBOOL *have)
{
	*have = TRUE;
	if (!win)                                 { *have = FALSE; return 0; }
	if (GUI_BG_IS_CLEAR(win->background))     return GUI_KEY_COLOR;
	if (GUI_COLOR_HAS(win->background))
		return RGB(GUI_COLOR_R(win->background),
		           GUI_COLOR_G(win->background),
		           GUI_COLOR_B(win->background));
	*have = FALSE;
	return 0;
}


/***********************************************************************
**  Default colours, and `dark-controls?`.
**
**  Win32's system colours do not change with the dark appearance -
**  COLOR_WINDOW stays white - and its controls have no documented dark
**  look. So a window follows the dark appearance only when a script
**  asks for it with `win/dark-controls?: true`; otherwise only its title
**  bar does, and everything inside keeps the light look.
**
**  With it on, and the system dark, everything left at its default
**  (`background: none`, `color: none`) resolves to these instead of the
**  system colours: the window's and the panels' fill, text, and a shade
**  lighter for what is typed in. A colour a script SET is never changed -
**  there is no knowing what its dark counterpart should be.
**
**  Dark_Now is the system-wide setting, read at start-up and again on
**  every switch; Dark_For() is whether one window is shown dark.
***********************************************************************/
static REBOOL Dark_Now = FALSE;
static REBOOL System_Dark(void);   // with the rest of the theme code

#define DARK_WINDOW  RGB(32, 32, 32)
#define DARK_ENTRY   RGB(45, 45, 45)   // a field, an area, a drop-down's list
#define DARK_TEXT    RGB(235, 235, 235)

static REBOOL Dark_For(GUIWIN *win)
{
	return (Dark_Now && win && (win->flags & GUIW_DARK_CONTROLS)) ? TRUE : FALSE;
}

static COLORREF Default_Window_Color(GUIWIN *win)
{
	return Dark_For(win) ? DARK_WINDOW : GetSysColor(COLOR_WINDOW);
}

static COLORREF Default_Text_Color(GUIWIN *win)
{
	return Dark_For(win) ? DARK_TEXT : GetSysColor(COLOR_WINDOWTEXT);
}

// Real brushes, never deleted by a caller: the system's cached one when
// light, one made here once when dark.
static HBRUSH Default_Window_Brush(GUIWIN *win)
{
	static HBRUSH dark = NULL;
	if (!Dark_For(win)) return GetSysColorBrush(COLOR_WINDOW);
	if (!dark) dark = CreateSolidBrush(DARK_WINDOW);
	return dark ? dark : GetSysColorBrush(COLOR_WINDOW);
}

static HBRUSH Default_Entry_Brush(GUIWIN *win)
{
	static HBRUSH dark = NULL;
	if (!Dark_For(win)) return GetSysColorBrush(COLOR_WINDOW);
	if (!dark) dark = CreateSolidBrush(DARK_ENTRY);
	return dark ? dark : GetSysColorBrush(COLOR_WINDOW);
}


/***********************************************************************
**  Fills `rect` of `dc` with what the WINDOW's client area is - its own
**  colour, the key colour when it is see-through, or the system window
**  colour. A panel with no colour of its own uses this too, which is
**  what keeps a panel invisible on a dark window rather than a pale
**  slab on it.
***********************************************************************/
static void Fill_Window_Background(HDC dc, const RECT *rect, GUIWIN *win)
{
	REBOOL   have = FALSE;
	COLORREF rgb  = Window_Fill_Color(win, &have);

	if (have) {
		HBRUSH brush = CreateSolidBrush(rgb);
		if (brush) {
			FillRect(dc, rect, brush);
			DeleteObject(brush);
			return;
		}
	}
	FillRect(dc, rect, Default_Window_Brush(win));
}

// The brush handed back for a widget with a background colour of its own.
// One slot, because WM_CTLCOLOR* is answered for one control at a time on
// this thread and the brush is used before the next answer is given.
static HBRUSH Ctl_Brush = NULL;

// Defined further down, and used before that: a transparent panel needs
// the first while painting itself, and a background change needs the
// second to reach a container's children.
static void Paint_Parent_Background(HWND hwnd, HDC dc);
static void Repaint_Widget(GUIWIDGET *wid, REBOOL now);


/***********************************************************************
**  The colour a transparent widget should show - when whatever holds it
**  has a flat one, which is nearly always.
**
**  Painting the container's own colour is INDISTINGUISHABLE from showing
**  through it, and it is worth a great deal: the control keeps erasing
**  itself normally, so it needs no subclass, and - the reason this
**  exists - the themed animation keeps working.
**
**  A themed check or radio cross-fades between states through
**  BufferedPaintAnimation, which paints into a memory DC and never
**  sends WM_ERASEBKGND. A control told "fill with nothing" therefore
**  animates from an empty buffer and vanishes for the length of the
**  fade. Handing it a real colour is what stops that.
**
**  Returns FALSE only when the answer is not a colour at all - an image
**  widget - where the pixels have to be fetched instead.
***********************************************************************/
static REBOOL Flat_Background_Of(GUIWIDGET *wid, COLORREF *rgb)
{
	GUIWIDGET *parent = wid ? (GUIWIDGET*)wid->parent : NULL;

	while (parent) {
		// Rendered pixels are not a colour.
		if (parent->kind == W_GUI_WIDGET_IMAGE) return FALSE;

		// A transparent container shows what IT sits on, so the question
		// moves up. A panel inside a panel inside the window ends here.
		if (GUI_BG_IS_CLEAR(parent->background)) {
			parent = (GUIWIDGET*)parent->parent;
			continue;
		}

		if (GUI_COLOR_HAS(parent->background)) {
			*rgb = RGB(GUI_COLOR_R(parent->background),
			           GUI_COLOR_G(parent->background),
			           GUI_COLOR_B(parent->background));
			return TRUE;
		}
		break; // a container with the platform's own background
	}

	// The window, or a container which left its background alone. A
	// SEE-THROUGH window is not a colour either - the key it fills with
	// is a colour the compositor removes, and a widget painting it would
	// have holes punched in it - so that falls to the render path.
	{	GUIWIN *owner = wid ? wid->owner : NULL;
		REBOOL  have  = FALSE;
		COLORREF own;

		if (owner && GUI_BG_IS_CLEAR(owner->background)) return FALSE;

		own = Window_Fill_Color(owner, &have);
		*rgb = have ? own : Default_Window_Color(owner);
	}
	return TRUE;
}


/***********************************************************************
**  Answering a control's WM_CTLCOLOR* message.
**
**  Win32 has no "text colour" property on a control: a control about to
**  paint asks its PARENT what to use, and this is that answer. The
**  colour therefore lives in the widget context, which is looked up
**  here from the child window the message came about.
**
**  Background stays COLOR_WINDOW throughout, which is what keeps a
**  label on the same background the window and the panels fill with.
***********************************************************************/
static LRESULT Ctl_Color(HDC dc, HWND child, GUIWIN *win, UINT msg)
{
	GUIWIDGET *wid = NULL;

	// The window's own widget list, rather than the child's GWLP_USERDATA.
	// Not every window that sends this is one of ours - a combo box has an
	// internal list box which sends WM_CTLCOLORLISTBOX in its own name, and
	// whatever sits in ITS user data is not a GUIWIDGET to be dereferenced.
	// The list is flat and window-wide, so one walk covers panels too.
	if (child && win) {
		GUIWIDGET *w = (GUIWIDGET*)win->widgets;
		for (; w; w = (GUIWIDGET*)w->next) {
			if ((HWND)w->handle == child) { wid = w; break; }
		}
	}

	SetTextColor(dc, (wid && GUI_COLOR_HAS(wid->color))
		? RGB(GUI_COLOR_R(wid->color),
		      GUI_COLOR_G(wid->color),
		      GUI_COLOR_B(wid->color))
		: Default_Text_Color(win));

	// A colour to fill with: either the widget's own, or - for a
	// transparent one over a container whose background IS a colour - that
	// container's, which looks the same and behaves far better. See
	// Flat_Background_Of().
	if (wid) {
		COLORREF rgb;
		REBOOL   have = FALSE;

		if (GUI_BG_IS_CLEAR(wid->background)) {
			have = Flat_Background_Of(wid, &rgb);
		} else if (GUI_COLOR_HAS(wid->background)) {
			rgb  = RGB(GUI_COLOR_R(wid->background),
			           GUI_COLOR_G(wid->background),
			           GUI_COLOR_B(wid->background));
			have = TRUE;
		}

		if (have) {
			// The brush has to outlive this return - the control fills
			// with it immediately afterwards - and only one control is
			// answered at a time on this thread, so one slot is enough.
			// The previous brush is released on the way in rather than
			// left to leak.
			if (Ctl_Brush) DeleteObject(Ctl_Brush);
			Ctl_Brush = CreateSolidBrush(rgb);
			if (Ctl_Brush) {
				SetBkColor(dc, rgb);
				return (LRESULT)Ctl_Brush;
			}
		}

		// Transparent over something which is not a colour - rendered
		// pixels. TRANSPARENT stops the control filling as it draws the
		// glyphs, and a hollow brush stops it filling the rest; what
		// shows through is what Transparent_Proc() put there.
		if (GUI_BG_IS_CLEAR(wid->background)) {
			SetBkMode(dc, TRANSPARENT);
			return (LRESULT)GetStockObject(NULL_BRUSH);
		}
	}

	// The platform's own, which here means the window's.
	//
	// A REAL brush handle. `(HBRUSH)(COLOR_WINDOW + 1)` is the encoding
	// WNDCLASS.hbrBackground and FillRect accept, and it is NOT a handle:
	// returned from here it is an invalid one, and the control fills with
	// whatever it falls back to - COLOR_3DFACE grey, darker than the
	// window. Visible under the classic look, where a static, a check and
	// a radio paint their own background with this brush, and hidden under
	// visual styles, where the theme paints it and the brush is never
	// used. That is why it only showed with the old look.
	//
	// GetSysColorBrush hands back a cached brush owned by the system: it
	// needs no cleanup and must not be deleted.
	{	REBOOL   have = FALSE;
		COLORREF own  = Window_Fill_Color(win, &have);
		if (have) {
			if (Ctl_Brush) DeleteObject(Ctl_Brush);
			Ctl_Brush = CreateSolidBrush(own);
			if (Ctl_Brush) {
				SetBkColor(dc, own);
				return (LRESULT)Ctl_Brush;
			}
		}
	}
	// Something to TYPE in - a field, an area, a drop-down's list - is a
	// shade lighter than a dark window, as Windows' own are, so that it
	// still reads as a place to type.
	if (Dark_For(win) && (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX)) {
		SetBkColor(dc, DARK_ENTRY);
		return (LRESULT)Default_Entry_Brush(win);
	}
	SetBkColor(dc, Default_Window_Color(win));
	return (LRESULT)Default_Window_Brush(win);
}


//== events ===================================================================

static REBINT Modifiers(void)
{
	REBINT flags = 0;
	if (GetKeyState(VK_SHIFT)   < 0) flags |= GUI_FLAG_SHIFT;
	if (GetKeyState(VK_CONTROL) < 0) flags |= GUI_FLAG_CONTROL;
	if (GetKeyState(VK_MENU)    < 0) flags |= GUI_FLAG_ALT;
	return flags;
}

/***********************************************************************
**  Keeping a reported position inside what reports it.
**
**  At a scale that is not a whole number, device pixels and logical
**  units do not line up, and every conversion rounds on its own: the
**  last pixel row of a 22 unit tall radio at 125% is 332.8 units down
**  the window, which rounds to 333, while the radio's own box rounds to
**  311 + 22 = 333 - one past its end. So a move the OS delivered to the
**  radio, because the pointer IS over it, reported a position a unit
**  outside it; and with the box's origin summed through nested
**  containers, the error can grow by a unit per level.
**
**  Windows' word on which control is under the pointer is the truth, so
**  the position is pulled back inside that control's box - never by
**  more than the rounding put it out. Not while the mouse is captured:
**  a drag goes wherever the pointer does, and is meant to.
***********************************************************************/
static void Clamp_Into(REBINT *x, REBINT *y, REBINT ax, REBINT ay, REBINT w, REBINT h)
{
	if (w > 0) { if (*x < ax) *x = ax; else if (*x > ax + w - 1) *x = ax + w - 1; }
	if (h > 0) { if (*y < ay) *y = ay; else if (*y > ay + h - 1) *y = ay + h - 1; }
}

static void Clamp_To_Widget(GUIWIDGET *wid, REBINT *x, REBINT *y)
{
	GUIWIDGET *at;
	REBINT ax = 0, ay = 0, w = 0, h = 0, bx, by, bw, bh;

	if (GetCapture() || !Gui_Widget_Get_Box(wid, &bx, &by, &w, &h)) return;
	// Its top-left corner in the window - the same sum `widget/at` makes,
	// so the clamped position and `at` agree to the unit.
	for (at = wid; at; at = (GUIWIDGET*)at->parent) {
		if (!Gui_Widget_Get_Box(at, &bx, &by, &bw, &bh)) return;
		ax += bx;
		ay += by;
	}
	Clamp_Into(x, y, ax, ay, w, h);
}

static void Clamp_To_Window(GUIWIN *win, REBINT *x, REBINT *y)
{
	REBINT w = 0, h = 0;
	if (GetCapture() || !Gui_Get_Size(win, &w, &h)) return;
	Clamp_Into(x, y, 0, 0, w, h);
}

// Queues a mouse event at the position carried by lParam.
static void Queue_Mouse(GUIWIN *win, REBCNT type, LPARAM lp, REBINT extra)
{
	int dpi;
	if (!win || !win->hob) return;
	// Reported in logical units, like every other coordinate here - at
	// the DPI of the monitor the window is on.
	REBINT x, y;
	dpi = Dpi_Of(HWND_OF(win));
	x = To_Logical(dpi, GET_X_LPARAM(lp));
	y = To_Logical(dpi, GET_Y_LPARAM(lp));
	Clamp_To_Window(win, &x, &y);
	Gui_Queue_Event(win->hob, type, x, y, Modifiers() | extra);
}

// The same for a child widget. A child covers its part of the window, so
// the parent stops hearing about the mouse there - the widget reports it
// instead, with itself as the source.
//
// The position is still the WINDOW's: every mouse event is in client
// coordinates of the window it happened in, whichever widget reports it,
// so a handler never has to know which one it was over to use it. lParam
// is in the child's own coordinates, so it is mapped up to the window
// first. `widget/at` converts back for a handler that wants that.
static void Queue_Widget_Mouse(GUIWIDGET *wid, REBCNT type, LPARAM lp, REBINT extra)
{
	POINT p;
	int   dpi;

	if (!wid || !wid->hob || !wid->handle) return;
	p.x = GET_X_LPARAM(lp);
	p.y = GET_Y_LPARAM(lp);
	if (wid->owner && wid->owner->handle)
		MapWindowPoints(HWND_OF_WID(wid), HWND_OF(wid->owner), &p, 1);
	REBINT x, y;
	dpi = Dpi_Of(HWND_OF_WID(wid));   // a control shares its window's DPI
	x = To_Logical(dpi, p.x);
	y = To_Logical(dpi, p.y);
	Clamp_To_Widget(wid, &x, &y);
	Gui_Queue_Event(wid->hob, type, x, y, Modifiers() | extra);
}


/***********************************************************************
**  Noticing that the pointer has left.
**
**  `enter` and `leave` are worked out in the shared layer from the
**  moves themselves - see Hover_To() in gui-commands.c. The one thing a
**  move cannot tell it is that the pointer went somewhere this program
**  hears nothing from, and that takes WM_MOUSELEAVE, which Windows only
**  sends to a window which asked for it: TrackMouseEvent, renewed on
**  every move (it is one-shot).
**
**  It is also sent when the pointer merely moves onto a CHILD of the
**  window, or back from one - the same window as far as the pointer is
**  concerned. The next move reports that, so a leave which still finds
**  the pointer over the same top-level window is ignored.
***********************************************************************/
static void Track_Leave(HWND hwnd)
{
	TRACKMOUSEEVENT t;
	t.cbSize      = sizeof(t);
	t.dwFlags     = TME_LEAVE;
	t.hwndTrack   = hwnd;
	t.dwHoverTime = 0;
	TrackMouseEvent(&t);
}

static void Mouse_Left(HWND hwnd)
{
	POINT p;
	HWND  under;

	if (GetCapture()) return;  // a press in progress keeps the pointer
	if (GetCursorPos(&p) && (under = WindowFromPoint(p)) != NULL
	    && GetAncestor(under, GA_ROOT) == GetAncestor(hwnd, GA_ROOT))
		return;
	Gui_Pointer_Left();
}


/***********************************************************************
**  Tooltips.
**
**  One tooltip control per top-level window, made the first time a
**  widget in it is given a tip, and owned by the window - so Windows
**  destroys it together with the window. It is kept as a property of
**  the window rather than in GUIWIN, which has no Win32 types in it.
**
**  Each widget is a tool identified by its own HWND (TTF_IDISHWND), and
**  the tooltip is fed its mouse messages by hand (TTM_RELAYEVENT) from
**  Nav_Proc and the window procedure. That is instead of TTF_SUBCLASS,
**  which would subclass every control a second time with comctl32's
**  own procedure on top of ours.
**
**  A LABEL cannot be done that way: a static answers HTTRANSPARENT, so
**  it never sees the mouse - its container does. A label's tip is a
**  RECTANGLE tool on its container instead, covering the label, which
**  the container's relayed messages hit. The rectangle is kept in step
**  when the label moves (Gui_Widget_Set_Box, WM_DPICHANGED).
***********************************************************************/
static const WCHAR *Tip_Prop = L"RebolGuiTip";

static HWND Tip_Of(HWND any, REBOOL make)
{
	HWND root = any ? GetAncestor(any, GA_ROOT) : NULL;
	HWND tip;

	if (!root) return NULL;
	tip = (HWND)GetPropW(root, Tip_Prop);
	if (tip || !make) return tip;

	tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
	                      // No fade in or out and no slide: the tip is there
	                      // when it is due and gone when it is not.
	                      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX
	                      | TTS_NOFADE | TTS_NOANIMATE,
	                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
	                      root, NULL, App_Instance, NULL);
	if (!tip) return NULL;
	// A width makes it wrap long text - and honour a newline in it.
	SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)To_Device(Dpi_Of(root), 400));
	SetPropW(root, Tip_Prop, tip);
	return tip;
}

// The tool describing `wid`. TTTOOLINFOW_V2_SIZE rather than sizeof: the
// full structure is only accepted by comctl32 6, which needs a manifest
// the `r3` executable may not have.
static void Tip_Tool(TOOLINFOW *ti, GUIWIDGET *wid)
{
	HWND hwnd = HWND_OF_WID(wid);

	ZeroMemory(ti, sizeof(*ti));
	ti->cbSize = TTTOOLINFOW_V2_SIZE;
	ti->hwnd   = GetParent(hwnd);
	ti->uId    = (UINT_PTR)hwnd;
	if (wid->kind == W_GUI_WIDGET_TEXT) {
		// Its rectangle in the container, which gets the label's mouse.
		if (GetWindowRect(hwnd, &ti->rect))
			MapWindowPoints(NULL, ti->hwnd, (POINT*)&ti->rect, 2);
	} else {
		ti->uFlags = TTF_IDISHWND;
	}
}

// Keeps a label's rectangle tool on top of the label. Nothing to do for
// any other kind, or for a label with no tip.
static void Tip_Follow(GUIWIDGET *wid)
{
	TOOLINFOW ti;
	HWND      tip;

	if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_TEXT) return;
	if (!(tip = Tip_Of(HWND_OF_WID(wid), FALSE))) return;
	Tip_Tool(&ti, wid);
	SendMessageW(tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

// Hands a mouse message on to the window's tooltip, if it has one.
static void Tip_Relay(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	HWND tip;
	MSG  m;

	switch (msg) {
	case WM_MOUSEMOVE:
	case WM_LBUTTONDOWN: case WM_LBUTTONUP:
	case WM_RBUTTONDOWN: case WM_RBUTTONUP:
	case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		break;
	default:
		return;
	}
	if (!(tip = Tip_Of(hwnd, FALSE))) return;

	m.hwnd    = hwnd;
	m.message = msg;
	m.wParam  = wp;
	m.lParam  = lp;
	m.time    = GetMessageTime();
	GetCursorPos(&m.pt);
	SendMessageW(tip, TTM_RELAYEVENT, 0, (LPARAM)&m);
}


//== window procedure =========================================================

/***********************************************************************
**  Light and dark appearance.
**
**  Windows has no API for "is the system dark": the setting is the
**  AppsUseLightTheme value in the user's Personalize key, which is what
**  every program reads. Loaded late from advapi32, which the extension
**  does not otherwise link.
**
**  A switch arrives as WM_SETTINGCHANGE with "ImmersiveColorSet" as its
**  string, several times over; the shared layer reports only a real
**  change.
**
**  Only the TITLE BAR follows, through DwmSetWindowAttribute - attribute
**  20 on current Windows, 19 on builds before 18985. The classic Win32
**  controls have no documented dark look, so everything inside the
**  window stays as it is: restyling it is the script's, on the event.
***********************************************************************/
typedef LONG (WINAPI *REGGETVALUEW_T)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
typedef HRESULT (WINAPI *DWMSETWINDOWATTRIBUTE_T)(HWND, DWORD, LPCVOID, DWORD);

static REBOOL System_Dark(void)
{
	static REGGETVALUEW_T get = NULL;
	static REBOOL looked = FALSE;
	DWORD light = 1, size = sizeof(light);

	if (!looked) {
		HMODULE advapi = LoadLibraryW(L"advapi32.dll");
		looked = TRUE;
		if (advapi) get = (REGGETVALUEW_T)GetProcAddress(advapi, "RegGetValueW");
	}
	if (!get) return FALSE;
	if (get(HKEY_CURRENT_USER,
	        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
	        L"AppsUseLightTheme", 0x00000010 /* RRF_RT_REG_DWORD */, NULL, &light, &size)
	    != ERROR_SUCCESS)
		return FALSE;   // no such setting: the system has no dark mode
	return light ? FALSE : TRUE;
}

static void Dark_Title_Bar(HWND hwnd, REBOOL dark)
{
	static DWMSETWINDOWATTRIBUTE_T set = NULL;
	static REBOOL looked = FALSE;
	BOOL on = dark ? TRUE : FALSE;

	if (!looked) {
		HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
		looked = TRUE;
		if (dwm) set = (DWMSETWINDOWATTRIBUTE_T)GetProcAddress(dwm, "DwmSetWindowAttribute");
	}
	if (!set || !hwnd) return;
	if (FAILED(set(hwnd, 20, &on, sizeof(on))))   // DWMWA_USE_IMMERSIVE_DARK_MODE
		set(hwnd, 19, &on, sizeof(on));           // ... before Windows 10 20H1
}

/***********************************************************************
**  The controls' own dark look, for a window with `dark-controls?`.
**
**  Win32 has no documented dark theme for its controls. What Windows'
**  own programs use are the visual style classes "DarkMode_Explorer"
**  and "DarkMode_CFD", which SetWindowTheme() can select for a control:
**  undocumented, but present since Windows 10 1809. Where a class does
**  not exist, the control keeps its normal look and nothing breaks.
**
**    push button, toggle,  DarkMode_Explorer
**    check, radio
**    field, area,          DarkMode_Explorer - dark scroll bars; fill
**    text-list             and text come from WM_CTLCOLOREDIT and
**                          WM_CTLCOLORLISTBOX
**    drop-down             DarkMode_CFD
**
**  A themed check or radio draws its label in the theme's own colour,
**  whatever WM_CTLCOLORSTATIC says - on some Windows versions that is
**  black, which a dark window does not show well. That is the one part
**  the platform does not let this file fix without drawing the control
**  itself.
**
**  Going back to light hands every control its normal theme again.
***********************************************************************/
typedef HRESULT (WINAPI *SETWINDOWTHEME_T)(HWND, LPCWSTR, LPCWSTR);

/***********************************************************************
**  The rest of Windows' own dark mode - undocumented, by ordinal.
**
**  A dark visual style class is not enough by itself: uxtheme only hands
**  a window its dark parts (scroll bars, the popup menus) once the
**  process has said it can take them and the window has opted in. These
**  are the calls Explorer, Notepad and every open-source dark-mode Win32
**  program use; they exist only by ordinal, and only from Windows 10
**  1809 (build 17763) - so they are looked up once, on a build new
**  enough, and anything missing simply leaves the light look.
**
**    #135  SetPreferredAppMode(AllowDark)  (1903+; AllowDarkModeForApp
**          with TRUE on 1809 - same ordinal, same argument value)
**    #133  AllowDarkModeForWindow(hwnd, on)
**    #136  FlushMenuThemes()               - popup menus pick it up
***********************************************************************/
typedef int  (WINAPI *SETPREFERREDAPPMODE_T)(int);
typedef BOOL (WINAPI *ALLOWDARKMODEFORWINDOW_T)(HWND, BOOL);
typedef void (WINAPI *FLUSHMENUTHEMES_T)(void);
typedef LONG (WINAPI *RTLGETVERSION_T)(OSVERSIONINFOW*);

static ALLOWDARKMODEFORWINDOW_T pAllowDarkModeForWindow = NULL;
static FLUSHMENUTHEMES_T        pFlushMenuThemes        = NULL;

static void Load_Dark_Mode(void)
{
	static REBOOL looked = FALSE;
	OSVERSIONINFOW  ver;
	RTLGETVERSION_T rtl;
	HMODULE         ux;

	if (looked) return;
	looked = TRUE;

	// Undocumented ordinals mean something else on other builds, so the
	// build is checked first - and asked of ntdll, since GetVersionEx
	// lies to a process without a compatibility manifest.
	rtl = (RTLGETVERSION_T)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
	ZeroMemory(&ver, sizeof(ver));
	ver.dwOSVersionInfoSize = sizeof(ver);
	if (!rtl || rtl(&ver) != 0 || ver.dwMajorVersion < 10 || ver.dwBuildNumber < 17763)
		return;

	ux = LoadLibraryW(L"uxtheme.dll");
	if (!ux) return;
	{
		SETPREFERREDAPPMODE_T mode =
			(SETPREFERREDAPPMODE_T)GetProcAddress(ux, MAKEINTRESOURCEA(135));
		if (mode) mode(1);   // AllowDark: dark where a window opts in
	}
	pAllowDarkModeForWindow = (ALLOWDARKMODEFORWINDOW_T)GetProcAddress(ux, MAKEINTRESOURCEA(133));
	pFlushMenuThemes        = (FLUSHMENUTHEMES_T)GetProcAddress(ux, MAKEINTRESOURCEA(136));
}

static void Allow_Dark(HWND hwnd, REBOOL dark)
{
	Load_Dark_Mode();
	if (pAllowDarkModeForWindow && hwnd) pAllowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
}


/***********************************************************************
**  The edge of a field or an area.
**
**  WS_EX_CLIENTEDGE is drawn in the non-client area, in the system's
**  light frame colours, and no visual style class changes it - a bright
**  rectangle round a dark box. So for a dark window the base procedure
**  paints the non-client area first (the scroll bars live there too),
**  and then the outer ring the edge occupies is painted over in a dark
**  frame colour.
***********************************************************************/
#define DARK_EDGE    RGB(80, 80, 80)
#define DARK_HOT     RGB(62, 62, 62)   // a menu bar item under the pointer
#define DARK_GRAYED  RGB(120, 120, 120)

static void Paint_Dark_Edge(HWND hwnd)
{
	RECT   r, inner;
	HDC    dc;
	HBRUSH brush;
	HRGN   ring, hole;
	int    ex, ey, dpi;

	if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE)) return;
	if (!GetWindowRect(hwnd, &r)) return;
	OffsetRect(&r, -r.left, -r.top);

	dpi = Dpi_Of(hwnd);
	ex  = Metric(dpi, SM_CXEDGE);
	ey  = Metric(dpi, SM_CYEDGE);
	inner = r;
	InflateRect(&inner, -ex, -ey);

	dc = GetWindowDC(hwnd);
	if (!dc) return;
	ring = CreateRectRgnIndirect(&r);
	hole = CreateRectRgnIndirect(&inner);
	brush = CreateSolidBrush(DARK_EDGE);
	if (ring && hole && brush) {
		CombineRgn(ring, ring, hole, RGN_DIFF);
		FillRgn(dc, ring, brush);
	}
	if (brush) DeleteObject(brush);
	if (hole)  DeleteObject(hole);
	if (ring)  DeleteObject(ring);
	ReleaseDC(hwnd, dc);
}


/***********************************************************************
**  The menu BAR in a dark window.
**
**  Popup menus go dark with the calls above; the bar itself never does -
**  Windows draws it in the light colours whatever the appearance. The
**  way round it, which is what Notepad++ and the open-source dark-mode
**  samples do, is two undocumented messages the window receives when
**  the bar is drawn: WM_UAHDRAWMENU (the whole bar's background) and
**  WM_UAHDRAWMENUITEM (one item). Answering them paints the bar dark;
**  not answering leaves Windows' own. One light line is left under the
**  bar even so, which is painted over after WM_NCPAINT / WM_NCACTIVATE.
***********************************************************************/
#define WM_UAHDRAWMENU      0x0091
#define WM_UAHDRAWMENUITEM  0x0092

typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef union {
	struct { DWORD cx; DWORD cy; } rgsizeBar[2];
	struct { DWORD cx; DWORD cy; } rgsizePopup[4];
} UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; } UAHMENUPOPUPMETRICS;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

static void Dark_Menu_Bar(HWND hwnd, const UAHMENU *um)
{
	MENUBARINFO mbi;
	RECT        win;
	HBRUSH      brush;

	ZeroMemory(&mbi, sizeof(mbi));
	mbi.cbSize = sizeof(mbi);
	if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi) || !GetWindowRect(hwnd, &win)) return;
	OffsetRect(&mbi.rcBar, -win.left, -win.top);
	brush = CreateSolidBrush(DARK_WINDOW);
	if (brush) {
		FillRect(um->hdc, &mbi.rcBar, brush);
		DeleteObject(brush);
	}
}

static void Dark_Menu_Item(const UAHDRAWMENUITEM *item)
{
	WCHAR          text[256];
	MENUITEMINFOW  mii;
	UINT           state = item->dis.itemState;
	UINT           format = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
	HBRUSH         brush;
	RECT           r = item->dis.rcItem;

	text[0] = 0;
	ZeroMemory(&mii, sizeof(mii));
	mii.cbSize     = sizeof(mii);
	mii.fMask      = MIIM_STRING;
	mii.dwTypeData = text;
	mii.cch        = (UINT)(sizeof(text) / sizeof(text[0])) - 1;
	GetMenuItemInfoW(item->um.hmenu, (UINT)item->umi.iPosition, TRUE, &mii);

	brush = CreateSolidBrush((state & (ODS_HOTLIGHT | ODS_SELECTED)) ? DARK_HOT : DARK_WINDOW);
	if (brush) {
		FillRect(item->um.hdc, &r, brush);
		DeleteObject(brush);
	}
	// Underlined access keys only when the keyboard asked for them, as
	// Windows' own bar does.
	if (state & ODS_NOACCEL) format |= DT_HIDEPREFIX;
	SetBkMode(item->um.hdc, TRANSPARENT);
	SetTextColor(item->um.hdc, (state & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE))
	                          ? DARK_GRAYED : DARK_TEXT);
	DrawTextW(item->um.hdc, text, -1, &r, format);
}

// The one light line Windows leaves between the bar and the client area.
static void Dark_Menu_Line(HWND hwnd)
{
	RECT   client, win, line;
	POINT  origin = {0, 0};
	HDC    dc;
	HBRUSH brush;

	if (!GetMenu(hwnd) || !GetClientRect(hwnd, &client) || !GetWindowRect(hwnd, &win)) return;
	ClientToScreen(hwnd, &origin);
	line.left   = origin.x - win.left;
	line.right  = line.left + (client.right - client.left);
	line.bottom = origin.y - win.top;
	line.top    = line.bottom - 1;

	dc = GetWindowDC(hwnd);
	if (!dc) return;
	brush = CreateSolidBrush(DARK_WINDOW);
	if (brush) {
		FillRect(dc, &line, brush);
		DeleteObject(brush);
	}
	ReleaseDC(hwnd, dc);
}

/***********************************************************************
**  Drawing what no dark style covers: a check's or a radio's label, a
**  slider, a progress bar.
**
**  A THEMED check or radio draws its label with the theme's own text
**  colour and ignores the one WM_CTLCOLORSTATIC hands it - black, which
**  a dark window hides. Its NM_CUSTOMDRAW is answered instead: the box or
**  the dot is still the theme's (DrawThemeBackground, in the state the
**  control is in), and only the label is drawn here, in the widget's own
**  colour or the dark default.
**
**  A trackbar has no dark style at all; its channel and thumb are drawn
**  here through its item custom draw. A progress bar has neither a dark
**  style nor custom draw, so a dark one is painted whole (in Nav_Proc).
**
**  All of it only for a window shown dark: everywhere else the controls
**  draw themselves exactly as before.
***********************************************************************/
typedef HANDLE  (WINAPI *OPENTHEMEDATA_T)(HWND, LPCWSTR);
typedef HRESULT (WINAPI *CLOSETHEMEDATA_T)(HANDLE);
typedef HRESULT (WINAPI *DRAWTHEMEBACKGROUND_T)(HANDLE, HDC, int, int, const RECT*, const RECT*);
typedef HRESULT (WINAPI *GETTHEMEPARTSIZE_T)(HANDLE, HDC, int, int, const RECT*, int, SIZE*);

static OPENTHEMEDATA_T       pOpenThemeData       = NULL;
static CLOSETHEMEDATA_T      pCloseThemeData      = NULL;
static DRAWTHEMEBACKGROUND_T pDrawThemeBackground = NULL;
static GETTHEMEPARTSIZE_T    pGetThemePartSize    = NULL;

static REBOOL Load_Theme_Drawing(void)
{
	static REBOOL looked = FALSE;
	if (!looked) {
		HMODULE ux = LoadLibraryW(L"uxtheme.dll");
		looked = TRUE;
		if (ux) {
			pOpenThemeData       = (OPENTHEMEDATA_T)GetProcAddress(ux, "OpenThemeData");
			pCloseThemeData      = (CLOSETHEMEDATA_T)GetProcAddress(ux, "CloseThemeData");
			pDrawThemeBackground = (DRAWTHEMEBACKGROUND_T)GetProcAddress(ux, "DrawThemeBackground");
			pGetThemePartSize    = (GETTHEMEPARTSIZE_T)GetProcAddress(ux, "GetThemePartSize");
		}
	}
	return (pOpenThemeData && pCloseThemeData && pDrawThemeBackground && pGetThemePartSize)
		? TRUE : FALSE;
}

#define DARK_ACCENT  RGB(76, 160, 255)  // the filled part of a progress bar
#define DARK_THUMB   RGB(190, 190, 190)

// The part and state numbers of the "Button" theme class (vssym32.h).
#define GUI_BP_RADIOBUTTON 2
#define GUI_BP_CHECKBOX    3

static LRESULT Dark_Check_Draw(GUIWIDGET *wid, NMCUSTOMDRAW *cd)
{
	HWND    hwnd = cd->hdr.hwndFrom;
	HDC     dc   = cd->hdc;
	HANDLE  theme;
	int     part, state, gap, len;
	UINT    item = cd->uItemState;
	REBOOL  on;
	SIZE    box = {13, 13};
	RECT    r = cd->rc, glyph, text;
	WCHAR  *caption = NULL;
	HFONT   font, old = NULL;
	COLORREF bg;
	LRESULT  ui;
	UINT    format = DT_SINGLELINE | DT_VCENTER | DT_LEFT;

	if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
	if (!Load_Theme_Drawing() || !(theme = pOpenThemeData(hwnd, L"Button")))
		return CDRF_DODEFAULT;

	// What it sits on, as the control itself would fill it.
	if (!GUI_COLOR_HAS(wid->background) && Flat_Background_Of(wid, &bg)) {
		HBRUSH b = CreateSolidBrush(bg);
		if (b) { FillRect(dc, &r, b); DeleteObject(b); }
	} else if (GUI_COLOR_HAS(wid->background)) {
		HBRUSH b = CreateSolidBrush(RGB(GUI_COLOR_R(wid->background),
		                                GUI_COLOR_G(wid->background),
		                                GUI_COLOR_B(wid->background)));
		if (b) { FillRect(dc, &r, b); DeleteObject(b); }
	}

	on   = (SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED) ? TRUE : FALSE;
	part = (wid->kind == W_GUI_WIDGET_RADIO) ? GUI_BP_RADIOBUTTON : GUI_BP_CHECKBOX;
	// Unchecked 1-4, checked 5-8: normal, hot, pressed, disabled.
	state = (item & CDIS_DISABLED) ? 4 : (item & CDIS_SELECTED) ? 3 : (item & CDIS_HOT) ? 2 : 1;
	if (on) state += 4;

	pGetThemePartSize(theme, dc, part, state, NULL, 1 /* TS_TRUE */, &box);
	glyph.left   = r.left;
	glyph.top    = r.top + ((r.bottom - r.top) - box.cy) / 2;
	glyph.right  = glyph.left + box.cx;
	glyph.bottom = glyph.top + box.cy;
	pDrawThemeBackground(theme, dc, part, state, &glyph, NULL);
	pCloseThemeData(theme);

	// The label, after the glyph and the gap the themed control leaves.
	gap  = To_Device(Dpi_Of(hwnd), 3);
	text = r;
	text.left = glyph.right + gap;

	len = GetWindowTextLengthW(hwnd);
	if (len > 0 && (caption = (WCHAR*)MAKE_MEM((len + 1) * sizeof(WCHAR))) != NULL) {
		len = GetWindowTextW(hwnd, caption, len + 1);
		font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
		if (font) old = (HFONT)SelectObject(dc, font);

		ui = SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0);
		if (ui & UISF_HIDEACCEL) format |= DT_HIDEPREFIX;

		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, (item & CDIS_DISABLED) ? DARK_GRAYED
			: GUI_COLOR_HAS(wid->color)
			? RGB(GUI_COLOR_R(wid->color), GUI_COLOR_G(wid->color), GUI_COLOR_B(wid->color))
			: DARK_TEXT);
		DrawTextW(dc, caption, len, &text, format);

		if ((item & CDIS_FOCUS) && !(ui & UISF_HIDEFOCUS)) {
			RECT f = text;
			DrawTextW(dc, caption, len, &f, format | DT_CALCRECT);
			f.top = text.top + ((text.bottom - text.top) - (f.bottom - f.top)) / 2;
			f.bottom = f.top + (f.bottom - f.top);
			InflateRect(&f, 1, 1);
			SetTextColor(dc, DARK_TEXT);
			DrawFocusRect(dc, &f);
		}
		if (old) SelectObject(dc, old);
		FREE_MEM(caption);
	}
	return CDRF_SKIPDEFAULT;
}

static LRESULT Dark_Slider_Draw(NMCUSTOMDRAW *cd)
{
	HBRUSH b;
	RECT   r = cd->rc;

	switch (cd->dwDrawStage) {
	case CDDS_PREPAINT:
		return CDRF_NOTIFYITEMDRAW;
	case CDDS_ITEMPREPAINT:
		if (cd->dwItemSpec == TBCD_CHANNEL) {
			b = CreateSolidBrush(DARK_EDGE);
			if (b) { FillRect(cd->hdc, &r, b); DeleteObject(b); }
			return CDRF_SKIPDEFAULT;
		}
		if (cd->dwItemSpec == TBCD_THUMB) {
			COLORREF c = (cd->uItemState & CDIS_DISABLED) ? DARK_GRAYED
			           : (cd->uItemState & (CDIS_HOT | CDIS_SELECTED)) ? DARK_TEXT
			           : DARK_THUMB;
			HPEN   pen = CreatePen(PS_SOLID, 1, c);
			HGDIOBJ op, ob;
			int    round = (r.right - r.left) < (r.bottom - r.top)
			             ? (r.right - r.left) : (r.bottom - r.top);
			b = CreateSolidBrush(c);
			if (pen && b) {
				op = SelectObject(cd->hdc, pen);
				ob = SelectObject(cd->hdc, b);
				RoundRect(cd->hdc, r.left, r.top, r.right, r.bottom, round / 2, round / 2);
				SelectObject(cd->hdc, op);
				SelectObject(cd->hdc, ob);
			}
			if (pen) DeleteObject(pen);
			if (b)   DeleteObject(b);
			return CDRF_SKIPDEFAULT;
		}
		return CDRF_DODEFAULT;
	}
	return CDRF_DODEFAULT;
}

// The window's own widget for a control HWND - asked of the window's list
// rather than the control's user data, as Ctl_Color() does.
static GUIWIDGET *Widget_Of(GUIWIN *win, HWND hwnd)
{
	GUIWIDGET *w;
	if (!win || !hwnd) return NULL;
	for (w = (GUIWIDGET*)win->widgets; w; w = (GUIWIDGET*)w->next)
		if ((HWND)w->handle == hwnd) return w;
	return NULL;
}

// WM_NOTIFY in the window procedure: NM_CUSTOMDRAW for a dark window.
static REBOOL Dark_Custom_Draw(GUIWIN *win, LPARAM lp, LRESULT *result)
{
	NMHDR      *hdr = (NMHDR*)lp;
	GUIWIDGET  *wid;

	if (!hdr || hdr->code != NM_CUSTOMDRAW || !Dark_For(win)) return FALSE;
	if (!(wid = Widget_Of(win, hdr->hwndFrom))) return FALSE;

	switch (wid->kind) {
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
		*result = Dark_Check_Draw(wid, (NMCUSTOMDRAW*)lp);
		return TRUE;
	case W_GUI_WIDGET_SLIDER:
		*result = Dark_Slider_Draw((NMCUSTOMDRAW*)lp);
		return TRUE;
	}
	return FALSE;
}

// A progress bar, painted whole: a dark track, the done part in an accent,
// and the same edge a field has.
static void Dark_Progress_Paint(HWND hwnd, HDC dc)
{
	RECT   r, done;
	HBRUSH b;
	int    lo, hi, pos;
	REBOOL vertical = (GetWindowLongPtrW(hwnd, GWL_STYLE) & PBS_VERTICAL) ? TRUE : FALSE;

	GetClientRect(hwnd, &r);
	b = CreateSolidBrush(DARK_EDGE);
	if (b) { FrameRect(dc, &r, b); DeleteObject(b); }
	InflateRect(&r, -1, -1);
	b = CreateSolidBrush(DARK_ENTRY);
	if (b) { FillRect(dc, &r, b); DeleteObject(b); }

	lo  = (int)SendMessageW(hwnd, PBM_GETRANGE, TRUE, 0);
	hi  = (int)SendMessageW(hwnd, PBM_GETRANGE, FALSE, 0);
	pos = (int)SendMessageW(hwnd, PBM_GETPOS, 0, 0);
	if (hi <= lo || pos <= lo) return;
	if (pos > hi) pos = hi;

	done = r;
	if (vertical) done.top   = r.bottom - MulDiv(r.bottom - r.top, pos - lo, hi - lo);
	else          done.right = r.left   + MulDiv(r.right - r.left, pos - lo, hi - lo);
	b = CreateSolidBrush(DARK_ACCENT);
	if (b) { FillRect(dc, &done, b); DeleteObject(b); }
}

static void Theme_Control(GUIWIDGET *wid, REBOOL dark)
{
	static SETWINDOWTHEME_T set = NULL;
	static REBOOL looked = FALSE;
	HWND hwnd;

	if (!looked) {
		HMODULE ux = LoadLibraryW(L"uxtheme.dll");
		looked = TRUE;
		if (ux) set = (SETWINDOWTHEME_T)GetProcAddress(ux, "SetWindowTheme");
	}
	if (!set || !wid || !wid->handle) return;
	hwnd = HWND_OF_WID(wid);
	Allow_Dark(hwnd, dark);   // or its scroll bars stay light

	switch (wid->kind) {
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_TOGGLE:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
	case W_GUI_WIDGET_FIELD:
	case W_GUI_WIDGET_AREA:
	case W_GUI_WIDGET_TEXT_LIST:
		set(hwnd, dark ? L"DarkMode_Explorer" : NULL, NULL);
		break;
	case W_GUI_WIDGET_DROP_DOWN:
		set(hwnd, dark ? L"DarkMode_CFD" : NULL, NULL);
		break;
	case W_GUI_WIDGET_SLIDER:
	case W_GUI_WIDGET_PROGRESS:
		// No style to switch - they are drawn by hand when dark - but a
		// trackbar keeps what it drew and repaints only when its own state
		// changes, so a plain invalidate leaves the old look until it is
		// touched. WM_THEMECHANGED is what makes it drop that and draw
		// again, the message SetWindowTheme() sends the others.
		SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
		InvalidateRect(hwnd, NULL, TRUE);
		break;
	default:
		break;
	}
}

// Every control in a window, and the window repainted - frames and all,
// since every default colour in it may just have changed.
static void Theme_Window(GUIWIN *win, REBOOL dark)
{
	GUIWIDGET *wid;
	HWND       hwnd;
	if (!win || !win->handle) return;
	hwnd = HWND_OF(win);

	// The window itself, for its popup menus; then the menus are told.
	Allow_Dark(hwnd, dark);
	if (pFlushMenuThemes) pFlushMenuThemes();

	for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next)
		Theme_Control(wid, dark);

	// Every frame again - the fields' edges and the menu bar are in the
	// non-client area, which invalidating alone does not repaint.
	for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next)
		if (wid->handle)
			SetWindowPos(HWND_OF_WID(wid), NULL, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	if (GetMenu(hwnd)) DrawMenuBar(hwnd);
	RedrawWindow(hwnd, NULL, NULL,
	             RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void Gui_Window_Dark_Controls(GUIWIN *win, REBOOL on)
{
	(void)on;   // the flag is already set; Dark_For() reads it
	Theme_Window(win, Dark_For(win));
}

REBOOL Gui_Window_Dark(GUIWIN *win)
{
	(void)win;       // system-wide on Windows
	return System_Dark();
}


/***********************************************************************
**  A window moved to a monitor with another scale.
**
**  WM_DPICHANGED arrives with the new DPI and a suggested window
**  rectangle, and leaves everything else to the program: every control
**  is still where it was in PIXELS, in a font made for the old DPI. So
**  the window is rescaled here, the way a script would expect from a
**  layout written in logical units - the same layout, at the new size:
**
**    - every widget's box (at any depth: the list is flat) is scaled
**      from the old DPI to the new, in its parent's client area;
**    - every font is remade at the new DPI, same point size;
**    - the window keeps its logical CLIENT size, framed for the new DPI,
**      at the top-left corner Windows suggested.
**
**  Children first, then the window: WM_SIZE from the window's own resize
**  then reports a `resize` whose logical size has not changed.
***********************************************************************/
static void Rescale_Window(GUIWIN *win, HWND hwnd, int was, int now, const RECT *suggested)
{
	GUIWIDGET *wid;
	RECT       client, r;
	int        cw = 0, ch = 0;

	if (was <= 0 || now <= 0) return;

	if (was != now) {
		for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next) {
			HWND     child, parent;
			HFONT    font;
			LOGFONTW lf;

			if (!wid->handle) continue;
			child  = HWND_OF_WID(wid);
			parent = GetParent(child);

			if (parent && GetWindowRect(child, &r)) {
				MapWindowPoints(NULL, parent, (POINT*)&r, 2);
				SetWindowPos(child, NULL,
				             MulDiv(r.left, now, was), MulDiv(r.top, now, was),
				             MulDiv(r.right - r.left, now, was),
				             MulDiv(r.bottom - r.top, now, was),
				             SWP_NOZORDER | SWP_NOACTIVATE);
				Tip_Follow(wid);
			}

			// Same face, same point size, same style - made for `now`.
			font = (HFONT)SendMessageW(child, WM_GETFONT, 0, 0);
			if (font && GetObjectW(font, sizeof(lf), &lf)) {
				int    px    = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
				int    pt    = MulDiv(px, 72, was);
				REBCNT style = ((lf.lfWeight >= FW_SEMIBOLD) ? GUI_FONT_BOLD : 0)
				             | (lf.lfItalic ? GUI_FONT_ITALIC : 0);
				HFONT  next  = Font_For(lf.lfFaceName, pt, style, now);
				if (next) SendMessageW(child, WM_SETFONT, (WPARAM)next, FALSE);
			} else {
				SendMessageW(child, WM_SETFONT, (WPARAM)Default_Font_At(now), FALSE);
			}
		}

		if (GetClientRect(hwnd, &client)) {
			cw = MulDiv(client.right - client.left, now, was);
			ch = MulDiv(client.bottom - client.top, now, was);
		}
	} else if (GetClientRect(hwnd, &client)) {
		cw = client.right - client.left;
		ch = client.bottom - client.top;
	}

	r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
	Adjust_Rect(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), GetMenu(hwnd) != NULL,
	            (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE), now);
	SetWindowPos(hwnd, NULL,
	             suggested ? suggested->left : 0, suggested ? suggested->top : 0,
	             r.right - r.left, r.bottom - r.top,
	             SWP_NOZORDER | SWP_NOACTIVATE | (suggested ? 0 : SWP_NOMOVE));

	// Measured, as everywhere else the frame changes: a menu bar can wrap
	// differently at another size.
	Keep_Client_Size(hwnd, cw, ch);
	Remember_Dpi(hwnd, now);
	RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static LRESULT CALLBACK Gui_Window_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	GUIWIN *win;

	// Attach the context before anything else can arrive, so that even the
	// creation-time messages find their window.
	if (msg == WM_NCCREATE) {
		CREATESTRUCTW *cs = (CREATESTRUCTW*)lp;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	win = GUIWIN_OF(hwnd);
	if (!win) return DefWindowProcW(hwnd, msg, wp, lp);

	Tip_Relay(hwnd, msg, wp, lp);  // a label's tip is the window's to show

	switch (msg) {

	case WM_MOUSEMOVE:
		Track_Leave(hwnd);
		Queue_Mouse(win, EVT_MOVE, lp, 0);
		return 0;

	case WM_MOUSELEAVE:
		Mouse_Left(hwnd);
		return 0;

	case WM_LBUTTONDBLCLK:
		Queue_Mouse(win, EVT_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONDOWN:
		Queue_Mouse(win, EVT_DOWN, lp, 0);
		SetFocus(hwnd);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONUP:
		Queue_Mouse(win, EVT_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_RBUTTONDBLCLK:
		Queue_Mouse(win, EVT_ALT_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_RBUTTONDOWN:
		Queue_Mouse(win, EVT_ALT_DOWN, lp, 0);
		SetFocus(hwnd);
		SetCapture(hwnd);
		return 0;
	case WM_RBUTTONUP:
		Queue_Mouse(win, EVT_ALT_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_MBUTTONDBLCLK:
		Queue_Mouse(win, EVT_AUX_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_MBUTTONDOWN:
		Queue_Mouse(win, EVT_AUX_DOWN, lp, 0);
		SetFocus(hwnd);
		SetCapture(hwnd);
		return 0;
	case WM_MBUTTONUP:
		Queue_Mouse(win, EVT_AUX_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_MOUSEWHEEL: {
		// Unlike the button messages, this one carries SCREEN coordinates.
		POINT pt;
		UINT  lines = 3;
		REBINT delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;

		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		if (lines == 0 || lines > 100) lines = 3; // also covers WHEEL_PAGESCROLL

		pt.x = GET_X_LPARAM(lp);
		pt.y = GET_Y_LPARAM(lp);
		ScreenToClient(hwnd, &pt);

		if (win->hob)
			Gui_Queue_Event(win->hob, EVT_SCROLL_LINE,
			                To_Logical(Dpi_Of(hwnd), pt.x), To_Logical(Dpi_Of(hwnd), pt.y),
			                delta * (REBINT)lines);
		return 0; }

	case WM_SETTINGCHANGE:
		if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
			REBOOL dark = System_Dark();
			REBOOL was  = Dark_For(win);
			Dark_Now = dark;
			Dark_Title_Bar(hwnd, dark);
			// A window which asked to follow is restyled - once per real
			// switch, since the broadcast comes several times, and before
			// the event, so a handler sees the new look.
			if (Dark_For(win) != was) Theme_Window(win, Dark_For(win));
			Gui_Theme_Changed(win, dark);
		}
		break;   // on to DefWindowProc, which has its own use for it

	case WM_NOTIFY: {
		LRESULT r;
		if (Dark_Custom_Draw(win, lp, &r)) return r;
		break; }

	// The menu bar and the line under it, for a window shown dark - see
	// Dark_Menu_Bar(). Anything else is Windows' own.
	case WM_UAHDRAWMENU:
		if (!Dark_For(win)) break;
		Dark_Menu_Bar(hwnd, (const UAHMENU*)lp);
		return TRUE;
	case WM_UAHDRAWMENUITEM:
		if (!Dark_For(win)) break;
		Dark_Menu_Item((const UAHDRAWMENUITEM*)lp);
		return TRUE;
	case WM_NCPAINT:
	case WM_NCACTIVATE: {
		LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
		if (Dark_For(win)) Dark_Menu_Line(hwnd);
		return r; }

	case WM_DPICHANGED:
		Rescale_Window(win, hwnd, Remembered_Dpi(hwnd), (int)HIWORD(wp), (const RECT*)lp);
		return 0;

	case WM_SIZE:
		if (wp != SIZE_MINIMIZED && win->hob)
			Gui_Queue_Event(win->hob, EVT_RESIZE,
			                To_Logical(Dpi_Of(hwnd), (REBINT)LOWORD(lp)),
			                To_Logical(Dpi_Of(hwnd), (REBINT)HIWORD(lp)), 0);
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	case WM_SYSCHAR:
		// The window itself has the focus when nothing inside it does -
		// a menu accelerator has to work there too.
		if (Gui_Handle_Key(hwnd, msg, wp, lp)) return 0;
		break;

	case WM_CLOSE:
		// Only reported - closing is Rebol's decision, and doing it here
		// would destroy a window whose handle is still in use.
		if (win->hob)
			Gui_Queue_Event(win->hob, EVT_CLOSE, 0, 0, 0);
		return 0;

	case WM_DROPFILES: {
		/***************************************************************
		**  Dropped files.
		**
		**  WM_DROPFILES is the simple half of Win32 drag and drop: the
		**  shell has already done the dragging, and this arrives as an
		**  ordinary posted message - so it is dispatched by our own pump
		**  and there is no modal loop to be careful of. Dropped TEXT is
		**  the other half and needs a registered IDropTarget, which is
		**  not implemented: `drop-text` exists all the way through this
		**  extension, but only macOS produces one today.
		**
		**  The paths are copied out here and converted to file! values
		**  later, in `poll-events` - the window procedure must not
		**  allocate a Rebol series, which is the rule the whole event
		**  queue is built around.
		***************************************************************/
		HDROP  hdrop = (HDROP)wp;
		UINT   count = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
		POINT  pt;
		GUIDROPDATA *data;
		REBHOB *target = win->hob;
		HWND    child;
		UINT    n;

		DragQueryPoint(hdrop, &pt);   // client coordinates of this window

		// A drop lands on whatever is under the pointer, so a file dropped
		// on a widget reports the widget - the same rule a click follows.
		child = ChildWindowFromPointEx(hwnd, pt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
		if (child && child != hwnd) {
			GUIWIDGET *wid = (GUIWIDGET*)GetWindowLongPtrW(child, GWLP_USERDATA);
			if (wid && wid->hob) target = wid->hob;
		}

		data = Gui_Drop_Payload(GUI_DROP_FILES, count * 160);
		if (data) {
			for (n = 0; n < count; n++) {
				WCHAR  wide[MAX_PATH * 2];
				REBYTE utf8[MAX_PATH * 6];
				REBCNT len;
				int    bytes;

				len = DragQueryFileW(hdrop, n, wide, (UINT)(sizeof(wide) / sizeof(WCHAR)));
				if (!len) continue;
				bytes = WideCharToMultiByte(CP_UTF8, 0, wide, (int)len,
				                            (char*)utf8, (int)sizeof(utf8), NULL, NULL);
				if (bytes > 0) Gui_Drop_Append(data, utf8, (REBCNT)bytes);
			}
			Gui_Queue_Drop(target, data, To_Logical(Dpi_Of(hwnd), pt.x),
			                             To_Logical(Dpi_Of(hwnd), pt.y));
		}

		DragFinish(hdrop);
		return 0; }

	case WM_COMMAND: {
		// Child controls report through their parent, and the child's HWND
		// arrives in lParam - which is why the control does not need an id.
		HWND child = (HWND)lp;
		GUIWIDGET *wid;
		REBCNT type;
		REBINT x = 0, y = 0, w = 0, h = 0;

		// A menu pick and an accelerator arrive here too, and are told
		// apart by having no control behind them: lParam is NULL, and the
		// notification code is 0 for a menu, 1 for an accelerator.
		if (!child && HIWORD(wp) <= 1) {
			Gui_Menu_Picked(win, (REBCNT)LOWORD(wp));
			return 0;
		}

		if (!child) break;

		wid = (GUIWIDGET*)GetWindowLongPtrW(child, GWLP_USERDATA);

		// Notification codes overlap between control families (BN_CLICKED
		// and CBN_ERRSPACE are both 0), so the combo box codes are only
		// read when the control really is one.
		if (wid && wid->kind == W_GUI_WIDGET_DROP_DOWN) {
			switch (HIWORD(wp)) {
			case CBN_SELCHANGE: type = EVT_CHANGE;  break;
			case CBN_SETFOCUS:  type = EVT_FOCUS;   break;
			case CBN_KILLFOCUS: type = EVT_UNFOCUS; break;
			default: goto not_handled;
			}
			if (wid->hob) {
				Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
				Gui_Queue_Event(wid->hob, type, x, y, Modifiers());
			}
			return 0;
		}
		// ... and the list box ones likewise: LBN_SELCHANGE is 1, which
		// is also a button's BN_PAINT.
		if (wid && wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			switch (HIWORD(wp)) {
			// Only a pick by the user; LB_SETCURSEL raises nothing, so
			// `list/index: n` is not reported back as an event.
			case LBN_SELCHANGE: type = EVT_CHANGE;  break;
			case LBN_SETFOCUS:  type = EVT_FOCUS;   break;
			case LBN_KILLFOCUS: type = EVT_UNFOCUS; break;
			default: goto not_handled;
			}
			if (wid->hob) {
				Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
				Gui_Queue_Event(wid->hob, type, x, y, Modifiers());
			}
			return 0;
		}

		switch (HIWORD(wp)) {
		case BN_CLICKED:   type = EVT_CLICK;   break;
		// SetWindowText raises EN_CHANGE as well, and reporting our own
		// writes back as user edits would turn every `field/text: ...`
		// into an event.
		case EN_CHANGE:    if (Setting_Text) return 0;
		                   type = EVT_CHANGE;  break;
		case EN_SETFOCUS:  type = EVT_FOCUS;   break;
		case EN_KILLFOCUS: type = EVT_UNFOCUS; break;
		default: goto not_handled;
		}

		if (wid && wid->hob) {
			// The position slot carries the widget's own offset - a
			// notification has no cursor position of its own.
			Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
			if (type == EVT_CLICK) {
				// Toggles settle their state before the event goes out.
				Gui_Widget_Activated(wid, x, y, Modifiers());
			} else {
				Gui_Queue_Event(wid->hob, type, x, y, Modifiers());
			}
		}
		return 0;
		not_handled: break; }

	// A trackbar reports through its parent, like the button family does -
	// but on the scroll messages rather than WM_COMMAND. lParam is the
	// control; a zero there is a real scrollbar, which we do not create.
	case WM_HSCROLL:
	case WM_VSCROLL: {
		HWND child = (HWND)lp;
		GUIWIDGET *wid;
		REBINT x = 0, y = 0, w = 0, h = 0;

		if (!child) break;

		wid = (GUIWIDGET*)GetWindowLongPtrW(child, GWLP_USERDATA);
		if (wid && wid->hob && wid->kind == W_GUI_WIDGET_SLIDER) {
			Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
			Gui_Queue_Event(wid->hob, EVT_CHANGE, x, y, Modifiers());
		}
		return 0; }

	// Static labels paint themselves onto whatever the parent supplies;
	// this is what keeps them on the same background WM_PAINT fills with.
	// It is also the only place a control's TEXT colour can be given, so
	// `widget/color` is answered here rather than stored in the control.
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX:
		return Ctl_Color((HDC)wp, (HWND)lp, win, msg);

	case WM_ERASEBKGND:
		return TRUE; // painted below, without the flicker

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		Fill_Window_Background(dc, &ps.rcPaint, win);
		EndPaint(hwnd, &ps);
		return 0; }

	// What a transparent child asks for. The whole client rect, not a
	// paint rect: the child shifted the origin of its own DC so that this
	// draws the part it covers, and clipping does the rest.
	case WM_PRINTCLIENT: {
		RECT rect;
		GetClientRect(hwnd, &rect);
		Fill_Window_Background((HDC)wp, &rect, win);
		return 0; }

	case WM_NCDESTROY:
		// The window is gone for good - whether we destroyed it or the
		// system did. Drop the queued events which point at the handle and
		// release the lock that kept it alive.
		//
		// The drop target goes FIRST, while the HWND is still valid: it
		// holds the GUIWIN this is about to finish with, and a registration
		// outliving its window is a dangling one.
		Gui_Window_Revoke_Drop(win);
		win->handle = NULL;
		win->flags &= ~GUIW_VISIBLE;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		// DestroyWindow destroys the menu it was given, so the handle is
		// dropped rather than freed - destroying it again would be a
		// double free. The accelerator table is ours and is not.
		win->menu = NULL;
		if (win->accel) {
			DestroyAcceleratorTable((HACCEL)win->accel);
			win->accel = NULL;
		}
		if (win->hob) Gui_Window_Closed(win->hob);
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wp, lp);
}


//== image widget procedure ===================================================
//
// A class of its own rather than an owner-drawn STATIC: the control paints
// itself straight from the image! series and reports its own mouse events,
// which is what makes it usable as a canvas.


/***********************************************************************
**  The image widget's pixels, into a DC the caller owns - WM_PAINT's and
**  a transparent child's WM_PRINTCLIENT alike.
***********************************************************************/
static void Paint_Image(HWND hwnd, HDC dc, GUIWIDGET *wid)
{
	RECT    rect;
	REBYTE *bits = NULL;
	REBINT  iw = 0, ih = 0;

	GetClientRect(hwnd, &rect);

	if (Gui_Widget_Pixels(wid, &bits, &iw, &ih)) {
		BITMAPINFO bmi;
		int mode;

		// image! is BGRA, which is exactly what a 32-bit BI_RGB DIB
		// is - so the pixels go to the screen untouched. A negative
		// height means the rows are stored top-down.
		ZeroMemory(&bmi, sizeof(bmi));
		bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth       = iw;
		bmi.bmiHeader.biHeight      = -ih;
		bmi.bmiHeader.biPlanes      = 1;
		bmi.bmiHeader.biBitCount    = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		mode = SetStretchBltMode(dc, COLORONCOLOR);
		StretchDIBits(dc,
			0, 0, rect.right, rect.bottom, // destination: the whole widget
			0, 0, iw, ih,                  // source: the whole image
			bits, &bmi, DIB_RGB_COLORS, SRCCOPY);
		SetStretchBltMode(dc, mode);
	} else {
		Fill_Window_Background(dc, &rect, wid ? wid->owner : NULL);
	}
}


static LRESULT CALLBACK Gui_Image_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	GUIWIDGET *wid;

	if (msg == WM_NCCREATE) {
		CREATESTRUCTW *cs = (CREATESTRUCTW*)lp;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return DefWindowProcW(hwnd, msg, wp, lp);
	}

	wid = (GUIWIDGET*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (!wid) return DefWindowProcW(hwnd, msg, wp, lp);

	switch (msg) {

	// An image widget can hold other widgets, so it is a container like a
	// panel and has to get out of their way the same: a control notifies
	// ITS parent, and the window's procedure is the one that knows what to
	// do with a notification.
	case WM_COMMAND:
	case WM_NOTIFY:     // custom draw of a control inside - see Dark_Custom_Draw()
	case WM_HSCROLL:
	case WM_VSCROLL:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX: {
		HWND parent = GetParent(hwnd);
		if (parent) return SendMessageW(parent, msg, wp, lp);
		break; }

	case WM_ERASEBKGND:
		return TRUE; // WM_PAINT covers every pixel

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		Paint_Image(hwnd, dc, wid);
		EndPaint(hwnd, &ps);
		return 0; }

	// A transparent child of an image widget asks for this, and the answer
	// is the image itself - which is the whole point of letting an image
	// hold widgets: a caption ON the rendered pixels.
	case WM_PRINTCLIENT:
		Paint_Image(hwnd, (HDC)wp, wid);
		return 0;

	// WM_MOUSEMOVE is not here: Nav_Proc, which every control has in
	// front of its own procedure, reports it for all of them alike.

	case WM_LBUTTONDBLCLK:
		Queue_Widget_Mouse(wid, EVT_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONDOWN:
		Queue_Widget_Mouse(wid, EVT_DOWN, lp, 0);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONUP:
		Queue_Widget_Mouse(wid, EVT_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_RBUTTONDOWN:
		Queue_Widget_Mouse(wid, EVT_ALT_DOWN, lp, 0);
		return 0;
	case WM_RBUTTONUP:
		Queue_Widget_Mouse(wid, EVT_ALT_UP, lp, 0);
		return 0;

	case WM_MBUTTONDOWN:
		Queue_Widget_Mouse(wid, EVT_AUX_DOWN, lp, 0);
		return 0;
	case WM_MBUTTONUP:
		Queue_Widget_Mouse(wid, EVT_AUX_UP, lp, 0);
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wp, lp);
}


// How far in from the left edge a framed panel's caption starts. The same
// number is used on macOS, so the two look alike even though each measures
// the text with its own font.
#define PANEL_CAPTION_X(dpi) To_Device((dpi), 9)

/***********************************************************************
**  Everything a panel draws, into a DC the caller owns.
**
**  Split out of WM_PAINT so that WM_PRINTCLIENT can produce exactly the
**  same pixels: a transparent child renders its parent's background into
**  its own DC, and "the panel's background" has to mean the caption and
**  the frame too, not just the fill.
***********************************************************************/
static void Paint_Panel(HWND hwnd, HDC dc)
{
	RECT   rect, frame, gap;
	GUIWIDGET *wid = (GUIWIDGET*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	int    caption_len = GetWindowTextLengthW(hwnd);
	WCHAR *caption = NULL;
	HFONT  font, old_font = NULL;
	SIZE   text_size = {0, 0};
	int    inset = 0;
	HBRUSH bg = NULL;          // a colour of its own, if it was given one
	HBRUSH fill;               // what the background and the caption gap use

	GetClientRect(hwnd, &rect);

	if (wid && GUI_BG_IS_CLEAR(wid->background)) {
		// Transparent: what shows through is whatever holds the panel,
		// which is how a captioned frame can be put over an image.
		Paint_Parent_Background(hwnd, dc);
		fill = NULL;
	} else {
		if (wid && GUI_COLOR_HAS(wid->background)) {
			bg = CreateSolidBrush(RGB(GUI_COLOR_R(wid->background),
			                          GUI_COLOR_G(wid->background),
			                          GUI_COLOR_B(wid->background)));
		} else {
			// Otherwise whatever the WINDOW paints, so a panel is a place
			// to put things rather than a visible slab on it - including
			// on a window with a colour of its own.
			REBOOL   have = FALSE;
			COLORREF rgb  = Window_Fill_Color(wid ? wid->owner : NULL, &have);
			if (have) bg = CreateSolidBrush(rgb);
		}
		fill = bg ? bg : Default_Window_Brush(wid ? wid->owner : NULL);
		FillRect(dc, &rect, fill);
	}

	if (!wid || !(wid->state & GUI_PANEL_EDGE)) {
		if (bg) DeleteObject(bg);
		return;
	}

	// The caption is kept as the panel's window text, and drawn with
	// the panel's own font - which is what makes `panel/text`,
	// `panel/font-size` and the rest work through the generic
	// accessors, with no special case above this file.
	font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
	if (!font) font = Default_Font_At(Dpi_Of(hwnd));
	if (font) old_font = (HFONT)SelectObject(dc, font);

	if (caption_len > 0) {
		caption = (WCHAR*)MAKE_MEM((caption_len + 1) * sizeof(WCHAR));
		if (caption) {
			caption_len = GetWindowTextW(hwnd, caption, caption_len + 1);
			// Without the extent there is no gap to leave and no line
			// to break, so an unmeasurable caption is simply not drawn
			// rather than drawn over the frame.
			if (!GetTextExtentPoint32W(dc, caption, caption_len, &text_size)
			    || text_size.cx <= 0) {
				FREE_MEM(caption);
				caption = NULL;
			} else {
				inset = text_size.cy / 2;
			}
		}
	}

	// The frame starts halfway down the caption, so the text sits ON the
	// line - a classic group box - and the whole thing stays inside the
	// panel's box, which is why no child ever has to move for it.
	frame = rect;
	frame.top += inset;
	DrawEdge(dc, &frame, EDGE_ETCHED, BF_RECT);

	if (caption) {
		// The line is broken by painting the background back over the
		// span the caption occupies. The panel owns that colour - it
		// filled the whole client area with it above - so this is exact
		// rather than a guess at what shows through.
		gap.left   = PANEL_CAPTION_X(Dpi_Of(hwnd)) - To_Device(Dpi_Of(hwnd), 2);
		gap.top    = rect.top;
		gap.right  = gap.left + text_size.cx + To_Device(Dpi_Of(hwnd), 4);
		gap.bottom = rect.top + text_size.cy;
		if (gap.right > rect.right) gap.right = rect.right;
		// A transparent panel has no colour to paint the line out with,
		// so the caption is drawn over an unbroken frame instead. The
		// alternative would be re-fetching the parent for one strip,
		// which is a lot of work to hide four pixels of etching.
		if (fill) FillRect(dc, &gap, fill);

		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, GUI_COLOR_HAS(wid->color)
			? RGB(GUI_COLOR_R(wid->color),
			      GUI_COLOR_G(wid->color),
			      GUI_COLOR_B(wid->color))
			: Default_Text_Color(wid->owner));
		TextOutW(dc, PANEL_CAPTION_X(Dpi_Of(hwnd)), rect.top, caption, caption_len);
		FREE_MEM(caption);
	}

	if (old_font) SelectObject(dc, old_font);
	if (bg) DeleteObject(bg);
}


//== panel procedure ==========================================================
//
// A container, and nothing more. The one thing it must do is get out of the
// way: a control reports to ITS parent, so everything a panel holds would
// notify the panel instead of the window, and the window's proc - which is
// where all the reporting lives - would never hear about it.
//
// So the notifications are passed straight up. The handlers there identify
// the control from lParam rather than from the window that received the
// message, so forwarding is all it takes.

static LRESULT CALLBACK Gui_Panel_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {

	case WM_COMMAND:
	case WM_NOTIFY:     // custom draw of a control inside - see Dark_Custom_Draw()
	case WM_HSCROLL:
	case WM_VSCROLL:
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN:
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORLISTBOX: {
		HWND parent = GetParent(hwnd);
		if (parent) return SendMessageW(parent, msg, wp, lp);
		break; }

	// A custom window class gets neither of these for free: DefWindowProc
	// stores no font, so a panel keeps its own in the class's extra word.
	// Without this, `panel/font-size` would be written and then read back
	// as whatever the shell's message font is.
	case WM_SETFONT:
		SetWindowLongPtrW(hwnd, 0, (LONG_PTR)wp);
		if (LOWORD(lp)) InvalidateRect(hwnd, NULL, TRUE);
		return 0;

	case WM_GETFONT:
		return (LRESULT)GetWindowLongPtrW(hwnd, 0);

	case WM_ERASEBKGND:
		return TRUE; // WM_PAINT covers it

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		Paint_Panel(hwnd, dc);
		EndPaint(hwnd, &ps);
		return 0; }

	// What a transparent child asks for: the same drawing, into the DC it
	// hands over, so that what shows through the child is what would have
	// been under it. The caption and the frame are included, which is why
	// this shares Paint_Panel() rather than just filling.
	case WM_PRINTCLIENT:
		Paint_Panel(hwnd, (HDC)wp);
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wp, lp);
}


//== class registration =======================================================

static REBOOL Register_Panel_Class(void)
{
	WNDCLASSEXW wc;

	if (Panel_Class_Registered) return TRUE;

	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc   = Gui_Panel_Proc;
	// One pointer of storage per panel, holding the HFONT it was given -
	// see WM_SETFONT in the procedure above.
	wc.cbWndExtra    = sizeof(LONG_PTR);
	wc.hInstance     = App_Instance;
	wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszClassName = Class_Name_Panel;

	if (!RegisterClassExW(&wc)) return FALSE;

	Panel_Class_Registered = TRUE;
	return TRUE;
}


static REBOOL Register_Image_Class(void)
{
	WNDCLASSEXW wc;

	if (Image_Class_Registered) return TRUE;

	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc   = Gui_Image_Proc;
	wc.hInstance     = App_Instance;
	wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = NULL; // WM_PAINT does it
	wc.lpszClassName = Class_Name_Image;

	if (!RegisterClassExW(&wc)) return FALSE;

	Image_Class_Registered = TRUE;
	return TRUE;
}


static REBOOL Register_Class(void)
{
	WNDCLASSEXW wc;

	if (Class_Registered) return TRUE;

	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize        = sizeof(wc);
	wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc   = Gui_Window_Proc;
	wc.hInstance     = App_Instance;
	wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
	wc.hbrBackground = NULL; // WM_PAINT does it
	wc.lpszClassName = Class_Name;
	wc.hIcon         = LoadIconW(App_Instance, MAKEINTRESOURCEW(101));

	if (!RegisterClassExW(&wc)) return FALSE;

	Class_Registered = TRUE;
	return TRUE;
}


//== platform API =============================================================

typedef HRESULT (WINAPI *SETPROCESSDPIAWARENESS_T)(int);
typedef BOOL    (WINAPI *SETPROCESSDPIAWARE_T)(void);

// DPI_AWARENESS_CONTEXT is a HANDLE; declared here by value so that this
// builds with SDK headers which predate it.
typedef HANDLE  GUI_DPI_CONTEXT;
#define GUI_DPI_CONTEXT_PER_MONITOR    ((GUI_DPI_CONTEXT)-3)
#define GUI_DPI_CONTEXT_PER_MONITOR_V2 ((GUI_DPI_CONTEXT)-4)
typedef BOOL            (WINAPI *SETPROCESSDPIAWARENESSCONTEXT_T)(GUI_DPI_CONTEXT);
typedef GUI_DPI_CONTEXT (WINAPI *SETTHREADDPIAWARENESSCONTEXT_T)(GUI_DPI_CONTEXT);
typedef GUI_DPI_CONTEXT (WINAPI *GETTHREADDPIAWARENESSCONTEXT_T)(void);
typedef int             (WINAPI *GETAWARENESSFROMDPIAWARENESSCONTEXT_T)(GUI_DPI_CONTEXT);

/***********************************************************************
**  One-time process setup.
**
**  NOTE: DPI awareness is a PROCESS wide setting, so importing this
**  module changes how the whole interpreter is scaled. It is done here
**  because it has to happen before the first window exists, and doing it
**  late is worse than doing it visibly.
***********************************************************************/
void Gui_Init_Platform(void)
{
	HMODULE shcore, user32;
	INITCOMMONCONTROLSEX controls;

	if (App_Instance == NULL) App_Instance = GetModuleHandleW(NULL);

	// The trackbar and the progress bar live in comctl32 and their classes
	// have to be registered before either can be created.
	controls.dwSize = sizeof(controls);
	controls.dwICC  = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&controls);

	/*******************************************************************
	**  OLE, for drag and drop.
	**
	**  RegisterDragDrop needs an initialised single-threaded apartment
	**  on the thread which owns the window. The interpreter may already
	**  have COM up: S_FALSE means it was already initialised compatibly
	**  and is not ours to shut down, and RPC_E_CHANGED_MODE means it is
	**  up as multi-threaded, where drag and drop cannot be registered at
	**  all - so drops fall back to WM_DROPFILES rather than failing the
	**  window.
	*******************************************************************/
	{
		HRESULT hr = OleInitialize(NULL);
		Ole_Ready = (hr == S_OK || hr == S_FALSE) ? TRUE : FALSE;
		Ole_Ours  = (hr == S_OK) ? TRUE : FALSE;
		if (!Ole_Ready) {
			// Said out loud rather than degraded quietly: a window which
			// then takes drops from Explorer and from nothing else looks
			// like a bug in the drop code, which is where the time goes.
			if (hr == RPC_E_CHANGED_MODE) {
				printf("GUI: COM is already initialised on this thread as "
				       "MULTI-THREADED, so OLE drag and drop cannot be "
				       "registered.\n"
				       "GUI: Drops fall back to WM_DROPFILES, which only "
				       "Explorer sends. The host should start COM on its GUI "
				       "thread with COINIT_APARTMENTTHREADED.\n");
			} else {
				printf("GUI: OleInitialize failed (0x%08lx) - drops fall back "
				       "to WM_DROPFILES, which only Explorer sends\n",
				       (unsigned long)hr);
			}
			fflush(stdout);
		}
	}

	/*******************************************************************
	**  DPI awareness: per monitor where Windows can, system otherwise.
	**
	**  PER_MONITOR_AWARE_V2 (Windows 10 1703+) is what makes each window
	**  keep the DPI of its own monitor, and it also scales the title bar
	**  and the themed parts of the common controls for us. Everything
	**  else - positions, sizes, fonts - this file scales itself, which
	**  is why nothing counts as per-monitor unless GetDpiForWindow() is
	**  there to ask.
	**
	**  The THREAD context is set as well as the process one. A host whose
	**  manifest already declared an awareness makes the process call fail
	**  (it can be set once), but a window takes its awareness from the
	**  thread that creates it - so this still gets per-monitor windows,
	**  and coordinates asked for from this thread come back in the same
	**  physical pixels those windows use.
	**
	**  user32 and shcore stay loaded: the late-bound calls below are used
	**  for the life of the process, and both are loaded anyway.
	*******************************************************************/
	user32 = GetModuleHandleW(L"user32.dll");
	shcore = LoadLibraryW(L"shcore.dll");
	if (user32) {
		SETPROCESSDPIAWARENESSCONTEXT_T set_process =
			(SETPROCESSDPIAWARENESSCONTEXT_T)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
		SETTHREADDPIAWARENESSCONTEXT_T set_thread =
			(SETTHREADDPIAWARENESSCONTEXT_T)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
		GETTHREADDPIAWARENESSCONTEXT_T get_thread =
			(GETTHREADDPIAWARENESSCONTEXT_T)GetProcAddress(user32, "GetThreadDpiAwarenessContext");
		GETAWARENESSFROMDPIAWARENESSCONTEXT_T awareness_of =
			(GETAWARENESSFROMDPIAWARENESSCONTEXT_T)GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext");

		pGetDpiForWindow          = (GETDPIFORWINDOW_T)GetProcAddress(user32, "GetDpiForWindow");
		pAdjustWindowRectExForDpi = (ADJUSTWINDOWRECTEXFORDPI_T)GetProcAddress(user32, "AdjustWindowRectExForDpi");
		pGetSystemMetricsForDpi   = (GETSYSTEMMETRICSFORDPI_T)GetProcAddress(user32, "GetSystemMetricsForDpi");
		if (shcore) pGetDpiForMonitor = (GETDPIFORMONITOR_T)GetProcAddress(shcore, "GetDpiForMonitor");

		if (set_process && set_thread && get_thread && awareness_of
		    && pGetDpiForWindow && pGetDpiForMonitor) {
			if (!set_process(GUI_DPI_CONTEXT_PER_MONITOR_V2))
				set_process(GUI_DPI_CONTEXT_PER_MONITOR);
			if (!set_thread(GUI_DPI_CONTEXT_PER_MONITOR_V2))
				set_thread(GUI_DPI_CONTEXT_PER_MONITOR);
			// 2 is DPI_AWARENESS_PER_MONITOR_AWARE, whichever version.
			Per_Monitor = (awareness_of(get_thread()) == 2) ? TRUE : FALSE;
		}
	}

	if (!Per_Monitor) {
		// Not available, or refused: system awareness, as before. The
		// ForDpi calls are still used where they exist - at the system
		// DPI they answer what the plain ones do.
		SETPROCESSDPIAWARENESS_T fn = shcore
			? (SETPROCESSDPIAWARENESS_T)GetProcAddress(shcore, "SetProcessDpiAwareness")
			: NULL;
		if (fn) {
			fn(1); // PROCESS_SYSTEM_DPI_AWARE
		} else if (user32) {
			SETPROCESSDPIAWARE_T old =
				(SETPROCESSDPIAWARE_T)GetProcAddress(user32, "SetProcessDPIAware");
			if (old) old();
		}
	}

	// AFTER declaring awareness - before it, the system reports a polite
	// 96 whatever it really is.
	Read_Screen_DPI();

	Dark_Now = System_Dark();
}


void Gui_Quit_Platform(void)
{
	// Windows still open at this point belong to a process which is going
	// away; the system reclaims them. Only the class needs unregistering,
	// and only so that a reloaded extension can register it again.
	if (Class_Registered) {
		UnregisterClassW(Class_Name, App_Instance);
		Class_Registered = FALSE;
	}
	if (Image_Class_Registered) {
		UnregisterClassW(Class_Name_Image, App_Instance);
		Image_Class_Registered = FALSE;
	}
	// Only if it was ours - the interpreter's own COM is not to be shut
	// down by an extension being unloaded.
	if (Ole_Ours) {
		OleUninitialize();
		Ole_Ours = Ole_Ready = FALSE;
	}
	if (Panel_Class_Registered) {
		UnregisterClassW(Class_Name_Panel, App_Instance);
		Panel_Class_Registered = FALSE;
	}
	Free_Font_Cache(); // every HFONT made for a `font` or `font-size`
	if (Default_Font && Default_Font_Owned) {
		DeleteObject(Default_Font); // a stock object must not be deleted
		Default_Font_Owned = FALSE;
	}
	Default_Font = NULL;
}


REBOOL Gui_Open_Window(GUIWIN *win, REBINT x, REBINT y, REBINT w, REBINT h,
                       const REBYTE *title, REBCNT title_len, REBCNT flags)
{
	HWND  hwnd;
	RECT  rect;
	WCHAR *wide;
	DWORD style   = WINDOW_STYLE;
	DWORD exstyle = WINDOW_EXSTYLE;
	int   dpi;
	REBINT want_w, want_h;

	if (!Register_Class()) return FALSE;

	// See-through: the client area is filled with a colour the compositor
	// drops, leaving the controls on it. See GUI_KEY_COLOR.
	if (flags & GUI_WIN_TRANSPARENT) exstyle |= WS_EX_LAYERED;

	// A borderless window is WS_POPUP: no caption and no frame, so the
	// resize bits would have nothing to attach to either.
	if (flags & GUI_WIN_BORDERLESS) {
		style = (style & ~(WINDOW_BORDER_BITS | WINDOW_RESIZE_BITS)) | WS_POPUP;
	} else if (flags & GUI_WIN_FIXED) {
		style &= ~WINDOW_RESIZE_BITS;
	}

	// Logical in, device out - see the note on logical units. The sentinel
	// is not a coordinate and must not be scaled.
	//
	// The size is converted at the DPI of the monitor the window will open
	// on, which is the one the position falls on - or the primary one, where
	// CW_USEDEFAULT puts a new window.
	if (x != GUI_DEFAULT_POS && y != GUI_DEFAULT_POS) {
		LONG     px, py;
		HMONITOR mon = NULL;
		Logical_To_Screen(x, y, &px, &py, &mon);
		x = (REBINT)px;
		y = (REBINT)py;
		dpi = Dpi_Of_Monitor(mon);
	} else {
		POINT origin = {0, 0};
		if (x != GUI_DEFAULT_POS) x = To_Device(Gui_DPI, x);
		if (y != GUI_DEFAULT_POS) y = To_Device(Gui_DPI, y);
		dpi = Dpi_Of_Monitor(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
	}
	want_w = w;
	want_h = h;
	w = To_Device(dpi, w);
	h = To_Device(dpi, h);

	// The requested size is the CLIENT size - grow it by the frame.
	rect.left = 0; rect.top = 0; rect.right = w; rect.bottom = h;
	Adjust_Rect(&rect, style, FALSE, exstyle, dpi);

	// A missing - or empty - title gets a neutral default rather than an
	// empty title bar.
	wide = To_Wide(title, title_len);

	hwnd = CreateWindowExW(
		exstyle,
		Class_Name,
		wide ? wide : L"Rebol",
		style,
		(x == GUI_DEFAULT_POS) ? CW_USEDEFAULT : x,
		(y == GUI_DEFAULT_POS) ? CW_USEDEFAULT : y,
		rect.right - rect.left,
		rect.bottom - rect.top,
		NULL, NULL, App_Instance,
		win // arrives as lpCreateParams in WM_NCCREATE
	);

	if (wide) FREE_MEM(wide);
	if (!hwnd) return FALSE;

	win->handle = (void*)hwnd;
	win->flags  = 0;

	// Windows may have put it somewhere else than asked - CW_USEDEFAULT, a
	// position off every monitor - and so at another DPI. The client size
	// is what was promised, so it is made right at the DPI it really has.
	if (Dpi_Of(hwnd) != dpi) {
		dpi = Dpi_Of(hwnd);
		rect.left = 0; rect.top = 0;
		rect.right = To_Device(dpi, want_w); rect.bottom = To_Device(dpi, want_h);
		Adjust_Rect(&rect, style, FALSE, exstyle, dpi);
		SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
		             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	}
	Remember_Dpi(hwnd, dpi);

	// The title bar in the system's appearance from the start.
	Dark_Title_Bar(hwnd, System_Dark());

	// The three states live in one field, so /transparent is recorded as
	// the same value `win/transparent?: true` would write - and applying
	// it is the same call, rather than a second path to keep in step.
	if (flags & GUI_WIN_TRANSPARENT) {
		win->background = GUI_BG_CLEAR;
		Gui_Window_Set_Background(win);
	}
	return TRUE;
}


/***********************************************************************
**  win->background, applied.
***********************************************************************/

//== drag and drop ============================================================
//
// Two protocols, and this implements the real one.
//
// DragAcceptFiles() only sets WS_EX_ACCEPTFILES, and WM_DROPFILES is then a
// COURTESY OF THE DRAG SOURCE: an application dragging files is expected to
// notice that style and post the message itself, with a DROPFILES structure
// in shared memory. Explorer still does, for compatibility going back to
// Windows 3.1. Anything written against OLE drag and drop - which is the
// documented way since Win32, and includes Total Commander, most archivers
// and every browser - calls DoDragDrop and talks only to an IDropTarget
// registered with RegisterDragDrop. With no such target the drop is simply
// refused and no message is sent, which is why the legacy path looked like
// it worked: it was being tested from the one source which still supports it.
//
// So a real IDropTarget is registered per window. It also gets three things
// the legacy protocol cannot express at all: CF_UNICODETEXT (so `drop-text`
// is not macOS-only), the drag-OVER feedback which tells the user whether a
// drop will be taken, and the position DURING the drag rather than after it.
//
// WM_DROPFILES is kept as a fallback: if OLE cannot be initialised on this
// thread the window falls back to DragAcceptFiles, and Explorer still works.

typedef struct Gui_Drop_Target {
	IDropTarget iface;   // FIRST: an IDropTarget* is a GUIDROPTARGET*
	LONG        refs;
	GUIWIN     *win;
	DWORD       effect;  // what the current drag would do; 0 when we take nothing
} GUIDROPTARGET;


/***********************************************************************
**  Does this data object carry something we take?
**
**  Files first: a drop which has both is a file drop, because that is
**  what the user thinks they are dragging.
***********************************************************************/
static REBCNT Drop_Format_Of(IDataObject *obj)
{
	FORMATETC fmt;

	fmt.ptd      = NULL;
	fmt.dwAspect = DVASPECT_CONTENT;
	fmt.lindex   = -1;
	fmt.tymed    = TYMED_HGLOBAL;

	fmt.cfFormat = CF_HDROP;
	if (IDataObject_QueryGetData(obj, &fmt) == S_OK) return GUI_DROP_FILES;

	fmt.cfFormat = CF_UNICODETEXT;
	if (IDataObject_QueryGetData(obj, &fmt) == S_OK) return GUI_DROP_TEXT;

	return 0;
}


// One UTF-16 string into the payload, as UTF-8.
static void Drop_Append_Wide(GUIDROPDATA *data, const WCHAR *wide, int len)
{
	int     bytes;
	REBYTE *utf8;

	if (len <= 0) return;
	bytes = WideCharToMultiByte(CP_UTF8, 0, wide, len, NULL, 0, NULL, NULL);
	if (bytes <= 0) return;

	utf8 = (REBYTE*)MAKE_MEM((size_t)bytes);
	if (!utf8) return;
	WideCharToMultiByte(CP_UTF8, 0, wide, len, (char*)utf8, bytes, NULL, NULL);
	Gui_Drop_Append(data, utf8, (REBCNT)bytes);
	FREE_MEM(utf8);
}


/***********************************************************************
**  Reads the content out of the data object into a payload.
**
**  Returns NULL when there is nothing to take, and the caller then
**  reports DROPEFFECT_NONE rather than pretending the drop succeeded.
***********************************************************************/
static GUIDROPDATA *Drop_Payload_Of(IDataObject *obj, REBCNT kind)
{
	FORMATETC    fmt;
	STGMEDIUM    med;
	GUIDROPDATA *data = NULL;

	fmt.ptd      = NULL;
	fmt.dwAspect = DVASPECT_CONTENT;
	fmt.lindex   = -1;
	fmt.tymed    = TYMED_HGLOBAL;
	fmt.cfFormat = (kind == GUI_DROP_TEXT) ? CF_UNICODETEXT : CF_HDROP;

	if (IDataObject_GetData(obj, &fmt, &med) != S_OK) return NULL;

	if (kind == GUI_DROP_TEXT) {
		const WCHAR *text = (const WCHAR*)GlobalLock(med.hGlobal);
		if (text) {
			data = Gui_Drop_Payload(GUI_DROP_TEXT, 0);
			if (data) Drop_Append_Wide(data, text, (int)wcslen(text));
			GlobalUnlock(med.hGlobal);
		}
	} else {
		HDROP hdrop = (HDROP)GlobalLock(med.hGlobal);
		if (hdrop) {
			UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
			UINT n;
			data = Gui_Drop_Payload(GUI_DROP_FILES, count * 160);
			if (data) {
				for (n = 0; n < count; n++) {
					WCHAR wide[MAX_PATH * 2];
					UINT  len = DragQueryFileW(hdrop, n, wide,
					                           (UINT)(sizeof(wide) / sizeof(WCHAR)));
					Drop_Append_Wide(data, wide, (int)len);
				}
			}
			GlobalUnlock(med.hGlobal);
		}
	}

	ReleaseStgMedium(&med);
	return data;
}


// The handle a drop on this window should be reported against: the widget
// under the pointer, or the window itself. `pt` is in SCREEN coordinates,
// which is what IDropTarget is given.
static REBHOB *Drop_Target_At(GUIWIN *win, POINTL pt, POINT *client)
{
	HWND  hwnd = HWND_OF(win);
	HWND  child;
	POINT p;

	p.x = pt.x;
	p.y = pt.y;
	ScreenToClient(hwnd, &p);
	*client = p;

	child = ChildWindowFromPointEx(hwnd, p, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
	if (child && child != hwnd) {
		GUIWIDGET *wid = (GUIWIDGET*)GetWindowLongPtrW(child, GWLP_USERDATA);
		if (wid && wid->hob) return wid->hob;
	}
	return win->hob;
}


//-- IUnknown -----------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Drop_QueryInterface(IDropTarget *self,
                                                     REFIID riid, void **out)
{
	if (!out) return E_POINTER;
	if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
		*out = self;
		IDropTarget_AddRef(self);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Drop_AddRef(IDropTarget *self)
{
	return (ULONG)InterlockedIncrement(&((GUIDROPTARGET*)self)->refs);
}

static ULONG STDMETHODCALLTYPE Drop_Release(IDropTarget *self)
{
	GUIDROPTARGET *t = (GUIDROPTARGET*)self;
	LONG refs = InterlockedDecrement(&t->refs);
	if (refs == 0) FREE_MEM(t);
	return (ULONG)refs;
}

//-- IDropTarget --------------------------------------------------------------

static HRESULT STDMETHODCALLTYPE Drop_DragEnter(IDropTarget *self,
                                                IDataObject *obj, DWORD keys,
                                                POINTL pt, DWORD *effect)
{
	GUIDROPTARGET *t = (GUIDROPTARGET*)self;

	// Decided once per drag and remembered: DragOver runs on every mouse
	// move, and asking the data object each time is needless traffic across
	// the process boundary.
	t->effect = (obj && Drop_Format_Of(obj)) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
	if (effect) *effect = t->effect;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Drop_DragOver(IDropTarget *self, DWORD keys,
                                               POINTL pt, DWORD *effect)
{
	if (effect) *effect = ((GUIDROPTARGET*)self)->effect;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Drop_DragLeave(IDropTarget *self)
{
	((GUIDROPTARGET*)self)->effect = DROPEFFECT_NONE;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Drop_Drop(IDropTarget *self, IDataObject *obj,
                                           DWORD keys, POINTL pt, DWORD *effect)
{
	GUIDROPTARGET *t = (GUIDROPTARGET*)self;
	GUIDROPDATA   *data;
	REBCNT         kind;
	REBHOB        *target;
	POINT          client;

	t->effect = DROPEFFECT_NONE;
	if (effect) *effect = DROPEFFECT_NONE;

	if (!obj || !t->win || !t->win->hob) return S_OK;
	if (!(kind = Drop_Format_Of(obj))) return S_OK;
	if (!(data = Drop_Payload_Of(obj, kind))) return S_OK;

	target = Drop_Target_At(t->win, pt, &client);
	{
		int dpi = Dpi_Of(HWND_OF(t->win));
		Gui_Queue_Drop(target, data, To_Logical(dpi, client.x), To_Logical(dpi, client.y));
	}

	if (effect) *effect = DROPEFFECT_COPY;
	return S_OK;
}

static IDropTargetVtbl Drop_Vtbl = {
	Drop_QueryInterface,
	Drop_AddRef,
	Drop_Release,
	Drop_DragEnter,
	Drop_DragOver,
	Drop_DragLeave,
	Drop_Drop
};

/***********************************************************************
**  Whether the window accepts drops.
**
**  The OLE path is the real one - see the note above the drop target.
**  DragAcceptFiles stays as a fallback for the case where OLE could not
**  be initialised on this thread: Explorer still works then, and a
**  window is never left looking as though it accepts drops when it
**  cannot, because both paths are turned on and off together.
**
**  RegisterDragDrop takes its own reference, so the one this function
**  creates is released here and the target is freed when the OS lets go
**  of it - which RevokeDragDrop is what triggers.
***********************************************************************/
/***********************************************************************
**  The keyboard focus.
**
**  SetFocus activates the window the control belongs to as a side
**  effect, which is what a caller wants and cannot usefully be asked
**  for separately.
**
**  A control which cannot take the focus - a label, a progress bar,
**  anything disabled - is refused rather than quietly doing nothing:
**  SetFocus would return the previous focus in both cases, so the
**  answer is taken by asking afterwards.
***********************************************************************/
REBOOL Gui_Window_Set_Focus(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;
	SetFocus(HWND_OF(win));
	return (GetFocus() == HWND_OF(win)) ? TRUE : FALSE;
}


REBOOL Gui_Widget_Set_Focus(GUIWIDGET *wid)
{
	HWND hwnd;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	// A disabled control takes the focus on some Windows versions and not
	// on others; refusing it here makes the answer the same everywhere.
	if (!IsWindowEnabled(hwnd)) return FALSE;

	SetFocus(hwnd);
	return (GetFocus() == hwnd) ? TRUE : FALSE;
}


REBOOL Gui_Widget_Has_Focus(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	return (GetFocus() == HWND_OF_WID(wid)) ? TRUE : FALSE;
}


void Gui_Window_Set_Drop(GUIWIN *win, REBOOL accept)
{
	if (!win) return;

	if (accept) {
		if (win->handle && Ole_Ready && !win->droptarget) {
			GUIDROPTARGET *t = (GUIDROPTARGET*)MAKE_CLEAR_MEM(sizeof(GUIDROPTARGET));
			if (t) {
				HRESULT hr;
				t->iface.lpVtbl = &Drop_Vtbl;
				t->refs = 1;
				t->win  = win;
				hr = RegisterDragDrop(HWND_OF(win), &t->iface);
				if (hr == S_OK) {
					win->droptarget = t;
				} else {
					printf("GUI: RegisterDragDrop failed (0x%08lx)\n",
					       (unsigned long)hr);
					fflush(stdout);
					IDropTarget_Release(&t->iface);
				}
			}
		}
		// Belt and braces: a source which only speaks the legacy protocol
		// still finds the style, whether or not the OLE target is up.
		if (win->handle) DragAcceptFiles(HWND_OF(win), TRUE);
		win->flags |= GUIW_ACCEPTS_DROP;
	}
	else {
		Gui_Window_Revoke_Drop(win);
		if (win->handle) DragAcceptFiles(HWND_OF(win), FALSE);
		win->flags &= ~GUIW_ACCEPTS_DROP;
	}
}


/***********************************************************************
**  Takes the drop target off a window, without changing what the
**  window says it accepts.
**
**  Separate because closing has to do it too: a registered target
**  outliving its HWND is a dangling registration, and the target holds
**  a GUIWIN pointer which is about to be freed.
***********************************************************************/
static void Gui_Window_Revoke_Drop(GUIWIN *win)
{
	GUIDROPTARGET *t;

	if (!win || !(t = (GUIDROPTARGET*)win->droptarget)) return;
	win->droptarget = NULL;

	if (win->handle) RevokeDragDrop(HWND_OF(win));
	IDropTarget_Release(&t->iface);
}


void Gui_Window_Set_Background(GUIWIN *win)
{
	HWND  hwnd;
	DWORD exstyle;
	REBOOL clear;

	if (!win || !win->handle) return;
	hwnd    = HWND_OF(win);
	clear   = GUI_BG_IS_CLEAR(win->background);
	exstyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

	if (clear) {
		if (!(exstyle & WS_EX_LAYERED))
			SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
			                  (LONG_PTR)(exstyle | WS_EX_LAYERED));
		// Only the key is dropped; alpha is left at fully opaque, so the
		// controls are not dimmed along with it.
		SetLayeredWindowAttributes(hwnd, GUI_KEY_COLOR, 255, LWA_COLORKEY);
	} else if (exstyle & WS_EX_LAYERED) {
		// Taking the style away is what makes the window solid again -
		// clearing the key alone would leave a layered window, which is
		// composited differently and needlessly.
		SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
		                  (LONG_PTR)(exstyle & ~WS_EX_LAYERED));
		SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
			| SWP_FRAMECHANGED);
	}

	// Every control on it may be showing this colour, and WS_CLIPCHILDREN
	// keeps a plain invalidation from reaching them.
	RedrawWindow(hwnd, NULL, NULL,
	             RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}


void Gui_Close_Window(GUIWIN *win)
{
	// DestroyWindow sends WM_NCDESTROY, and that is where win->handle is
	// cleared and the handle context unlocked - one path for every way a
	// window can disappear.
	if (win && win->handle) DestroyWindow(HWND_OF(win));
}


void Gui_Show_Window(GUIWIN *win, REBOOL show)
{
	if (!win || !win->handle) return;
	ShowWindow(HWND_OF(win), show ? SW_SHOWNORMAL : SW_HIDE);
	if (show) {
		// RDW_ALLCHILDREN, and not a plain UpdateWindow: the controls were
		// invalidated as they were created and are waiting for the pump,
		// so a window shown after its layout was built has to paint them
		// along with itself. That is what makes
		//
		//     win: open-window/hidden ... ; add-* ... ; show-window win
		//
		// appear complete in one go instead of a frame at a time.
		RedrawWindow(HWND_OF(win), NULL, NULL,
		             RDW_INVALIDATE | RDW_ERASE | RDW_FRAME
		             | RDW_ALLCHILDREN | RDW_UPDATENOW);
		SetForegroundWindow(HWND_OF(win));
		win->flags |= GUIW_VISIBLE;
	} else {
		win->flags &= ~GUIW_VISIBLE;
	}
}


/***********************************************************************
**  Drains the thread's message queue.
**
**  NOTE: this takes messages for EVERY window of the calling thread, not
**  only ours. That is what makes a plain `poll-events` loop work without
**  a host side event device - but it also means two extensions pumping
**  the same thread would steal each other's messages.
***********************************************************************/
// The GUIWIN behind a top-level window, or NULL for a window which is not
// one of ours. The class name is the test: GWLP_USERDATA on a window this
// extension did not create holds whatever its owner put there, which is
// not a GUIWIN to be dereferenced.
static GUIWIN* Our_Window(HWND hwnd)
{
	WCHAR cls[64];

	if (!hwnd) return NULL;
	if (!GetClassNameW(hwnd, cls, 64)) return NULL;
	if (lstrcmpW(cls, Class_Name) != 0) return NULL;
	return GUIWIN_OF(hwnd);
}


REBCNT Gui_Pump(void)
{
	MSG msg;
	REBCNT dispatched = 0;

	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
		dispatched++;

		// A keyboard shortcut is not a property of a menu item on Win32 -
		// it is an entry in an accelerator table which SOMETHING has to
		// translate before the keystroke is dispatched, and this is the
		// only loop this extension owns. The message may have been aimed
		// at a control, so the window it belongs to is the root above it.
		//
		// Only for keyboard messages. Everything below is two USER32 calls
		// per message, and a themed control produces a great many messages
		// - animation timers, mouse tracking, buffered paint - none of
		// which can possibly be a shortcut.
		if (msg.message == WM_KEYDOWN    || msg.message == WM_SYSKEYDOWN
		 || msg.message == WM_KEYUP      || msg.message == WM_SYSKEYUP
		 || msg.message == WM_CHAR       || msg.message == WM_SYSCHAR) {
			/***********************************************************
			**  A FALLBACK, not the mechanism.
			**
			**  The keyboard normally never gets here at all: the host
			**  drains and dispatches the OS queue itself, so a keystroke
			**  reaches the focused control without passing through this
			**  loop - which is why the real handling is in the control,
			**  in Nav_Proc. This covers the other case, a program which
			**  drives `poll-events` in a loop of its own and never waits,
			**  where these ARE the messages nobody else has taken.
			***********************************************************/
			if (Gui_Handle_Key(msg.hwnd, msg.message, msg.wParam, msg.lParam))
				continue;
		}

		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	return dispatched;
}


REBOOL Gui_Get_Size(GUIWIN *win, REBINT *w, REBINT *h)
{
	RECT r;
	int  dpi;
	if (!win || !win->handle || !GetClientRect(HWND_OF(win), &r)) return FALSE;
	dpi = Dpi_Of(HWND_OF(win));
	*w = To_Logical(dpi, r.right - r.left);
	*h = To_Logical(dpi, r.bottom - r.top);
	return TRUE;
}


REBOOL Gui_Get_Offset(GUIWIN *win, REBINT *x, REBINT *y)
{
	RECT r;
	if (!win || !win->handle || !GetWindowRect(HWND_OF(win), &r)) return FALSE;
	// A position on the desktop, which spans monitors - see the note on
	// Screen_To_Logical() for how that is made logical.
	Screen_To_Logical(r.left, r.top, x, y);
	return TRUE;
}


REBOOL Gui_Set_Size(GUIWIN *win, REBINT w, REBINT h)
{
	RECT r;
	int  dpi;
	if (!win || !win->handle) return FALSE;

	dpi = Dpi_Of(HWND_OF(win));
	r.left = 0; r.top = 0; r.right = To_Device(dpi, w); r.bottom = To_Device(dpi, h);
	Adjust_Rect(&r, (DWORD)GetWindowLongPtrW(HWND_OF(win), GWL_STYLE),
	            FALSE, (DWORD)GetWindowLongPtrW(HWND_OF(win), GWL_EXSTYLE), dpi);

	return SetWindowPos(HWND_OF(win), NULL, 0, 0,
	                    r.right - r.left, r.bottom - r.top,
	                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ? TRUE : FALSE;
}


REBOOL Gui_Set_Offset(GUIWIN *win, REBINT x, REBINT y)
{
	LONG px, py;
	if (!win || !win->handle) return FALSE;
	// Moving onto a monitor with another scale raises WM_DPICHANGED, which
	// rescales the window there - its logical size is kept.
	Logical_To_Screen(x, y, &px, &py, NULL);
	return SetWindowPos(HWND_OF(win), NULL, (int)px, (int)py, 0, 0,
	                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ? TRUE : FALSE;
}


// The client size before the frame changes - a menu bar appearing, a
// border going away - so that it can be given straight back.
static REBOOL Client_Size_Of(HWND hwnd, int *w, int *h)
{
	RECT r;
	if (!GetClientRect(hwnd, &r)) return FALSE;
	*w = r.right - r.left;
	*h = r.bottom - r.top;
	return TRUE;
}

/***********************************************************************
**  Puts back whatever the frame took.
**
**  Neither SetMenu() nor a style change resizes a window - they
**  re-split it, so a menu bar appears, or a border grows, by taking the
**  room out of the CLIENT area. Everything a caller laid out is
**  positioned in that area, so the window is grown by exactly what was
**  lost and the layout does not move.
**
**  Measured rather than computed with AdjustWindowRect: a menu bar can
**  wrap onto two rows, and the measurement is right either way.
***********************************************************************/
static void Keep_Client_Size(HWND hwnd, int was_w, int was_h)
{
	RECT r;
	int now_w, now_h;

	if (!Client_Size_Of(hwnd, &now_w, &now_h)) return;
	if ((now_w == was_w && now_h == was_h) || !GetWindowRect(hwnd, &r)) return;

	SetWindowPos(hwnd, NULL, 0, 0,
		(r.right  - r.left) + (was_w - now_w),
		(r.bottom - r.top)  + (was_h - now_h),
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}


/***********************************************************************
**  The frame.
**
**  Read from the window rather than remembered, so what is reported is
**  what the window has - including a style someone else changed.
**
**  Changing it re-splits the window the way a menu bar does: the frame
**  grows or shrinks and the CLIENT area gives up or gains the
**  difference. Since everything in the window is laid out in that area,
**  the window is resized by what was lost, measured rather than
**  computed - Keep_Client_Size() again.
***********************************************************************/
static REBOOL Set_Window_Style_Bits(GUIWIN *win, DWORD off, DWORD on)
{
	HWND  hwnd;
	DWORD style, next;
	int   w, h;

	if (!win || !win->handle) return FALSE;
	hwnd = HWND_OF(win);

	style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
	next  = (style & ~off) | on;
	if (next == style) return TRUE;

	if (!Client_Size_Of(hwnd, &w, &h)) { w = h = 0; }

	SetWindowLongPtrW(hwnd, GWL_STYLE, (LONG_PTR)next);
	// SWP_FRAMECHANGED - without it the new style is stored but the frame
	// on screen is still the old one until something else recalculates it.
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
		| SWP_FRAMECHANGED);

	if (w && h) Keep_Client_Size(hwnd, w, h);
	return TRUE;
}


REBOOL Gui_Get_Resizable(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;
	return (GetWindowLongPtrW(HWND_OF(win), GWL_STYLE) & WS_THICKFRAME)
		? TRUE : FALSE;
}


REBOOL Gui_Set_Resizable(GUIWIN *win, REBOOL on)
{
	// A borderless window has no frame to grab, so this is only about the
	// bits; turning it on there does nothing visible until a border is.
	return on
		? Set_Window_Style_Bits(win, 0, WINDOW_RESIZE_BITS)
		: Set_Window_Style_Bits(win, WINDOW_RESIZE_BITS, 0);
}


REBOOL Gui_Get_Border(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;
	return (GetWindowLongPtrW(HWND_OF(win), GWL_STYLE) & WS_CAPTION)
		? TRUE : FALSE;
}


REBOOL Gui_Set_Border(GUIWIN *win, REBOOL on)
{
	if (!win) return FALSE;

	if (on) {
		// The resize bits are not restored here: whether the window can be
		// resized is its own property, and one it may never have had.
		return Set_Window_Style_Bits(win, WS_POPUP, WINDOW_BORDER_BITS);
	}
	return Set_Window_Style_Bits(win,
		WINDOW_BORDER_BITS | WINDOW_RESIZE_BITS, WS_POPUP);
}


REBSER* Gui_Get_Title(GUIWIN *win)
{
	if (!win || !win->handle) return NULL;
	return Text_Of(HWND_OF(win));
}


REBOOL Gui_Set_Title(GUIWIN *win, const REBYTE *utf8, REBCNT len)
{
	if (!win || !win->handle) return FALSE;
	return Set_Text_Of(HWND_OF(win), utf8, len);
}


//== menu bar =================================================================
//
// A Win32 menu belongs to the window, which is the easy half. The awkward
// halves are that a menu bar EATS CLIENT AREA - so a window given one would
// silently shrink under everything already laid out in it - and that a
// shortcut is not a property of a menu item at all, but an entry in a
// separate accelerator table which the message loop has to translate.

// Accelerators collected between Gui_Menu_Begin() and Gui_Menu_End(). One
// menu is built at a time, from a single thread, so a static is enough and
// nothing has to be grown per window.
#define GUI_MAX_ACCEL 128
static ACCEL Accel_Build[GUI_MAX_ACCEL];
static int   Accel_Count = 0;


REBOOL Gui_Menu_Begin(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;

	Accel_Count = 0;
	win->menu = (void*)CreateMenu();
	return win->menu != NULL;
}


void* Gui_Menu_Add_Popup(GUIWIN *win, void *parent,
                         const REBYTE *label, REBCNT len)
{
	HMENU  popup;
	WCHAR *wide;

	if (!win || !win->menu) return NULL;

	popup = CreatePopupMenu();
	if (!popup) return NULL;

	wide = To_Wide(label, len);
	AppendMenuW(parent ? (HMENU)parent : (HMENU)win->menu,
	            MF_STRING | MF_POPUP, (UINT_PTR)popup, wide ? wide : L"");
	if (wide) FREE_MEM(wide);

	return (void*)popup;
}


// "Ctrl+Shift+S", appended after a tab so that the menu right-aligns it.
// Windows does not read the accelerator table to label an item - the text
// is just text, and keeping the two in step is the caller's job, which
// here means this function's.
static void Append_Accel_Text(WCHAR *dst, size_t max, REBCNT key, REBCNT mods)
{
	WCHAR tail[40];
	int   n;

	lstrcpyW(tail, L"\tCtrl+");
	if (mods & GUI_FLAG_SHIFT) lstrcatW(tail, L"Shift+");
	if (mods & GUI_FLAG_ALT)   lstrcatW(tail, L"Alt+");
	n = lstrlenW(tail);
	tail[n++] = (WCHAR)((key >= 'a' && key <= 'z') ? key - 32 : key);
	tail[n] = 0;

	if ((size_t)(lstrlenW(dst) + lstrlenW(tail)) < max) lstrcatW(dst, tail);
}


void Gui_Menu_Add_Item(GUIWIN *win, void *parent,
                       const REBYTE *label, REBCNT len, REBCNT item_id,
                       REBCNT key, REBCNT mods)
{
	WCHAR  text[256];
	WCHAR *wide;

	if (!win || !win->menu) return;

	wide = To_Wide(label, len);
	lstrcpynW(text, wide ? wide : L"", 256);
	if (wide) FREE_MEM(wide);

	if (key) {
		Append_Accel_Text(text, 256, key, mods);

		if (Accel_Count < GUI_MAX_ACCEL) {
			ACCEL *a = &Accel_Build[Accel_Count++];
			a->fVirt = FVIRTKEY | FCONTROL;
			if (mods & GUI_FLAG_SHIFT) a->fVirt |= FSHIFT;
			if (mods & GUI_FLAG_ALT)   a->fVirt |= FALT;
			// A letter or a digit IS its own virtual key; anything else is
			// asked of the current keyboard layout.
			if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
				a->key = (WORD)key;
			} else if (key >= 'a' && key <= 'z') {
				a->key = (WORD)(key - 32);
			} else {
				a->key = (WORD)(VkKeyScanW((WCHAR)key) & 0xFF);
			}
			a->cmd = (WORD)item_id;
		}
	}

	AppendMenuW(parent ? (HMENU)parent : (HMENU)win->menu,
	            MF_STRING, (UINT_PTR)item_id, text);
}


void Gui_Menu_Add_Separator(GUIWIN *win, void *parent)
{
	if (!win || !win->menu) return;
	AppendMenuW(parent ? (HMENU)parent : (HMENU)win->menu,
	            MF_SEPARATOR, 0, NULL);
}


REBOOL Gui_Menu_End(GUIWIN *win)
{
	HWND hwnd;
	int  w, h;

	if (!win || !win->handle || !win->menu) return FALSE;
	hwnd = HWND_OF(win);

	if (!Client_Size_Of(hwnd, &w, &h)) { w = h = 0; }
	if (!SetMenu(hwnd, (HMENU)win->menu)) return FALSE;
	DrawMenuBar(hwnd);
	if (w && h) Keep_Client_Size(hwnd, w, h);

	if (Accel_Count > 0) {
		win->accel = (void*)CreateAcceleratorTableW(Accel_Build, Accel_Count);
	}
	Accel_Count = 0;
	return TRUE;
}


void Gui_Menu_Free(GUIWIN *win)
{
	if (!win) return;

	if (win->handle) {
		HWND hwnd = HWND_OF(win);
		int  w, h;
		if (win->menu) {
			if (!Client_Size_Of(hwnd, &w, &h)) { w = h = 0; }
			SetMenu(hwnd, NULL);
			DrawMenuBar(hwnd);
			// The room the bar was taking is given back the same way it
			// was taken, so removing a menu does not move anything either.
			if (w && h) Keep_Client_Size(hwnd, w, h);
		}
	}

	// A submenu is destroyed with the menu holding it, so the bar is the
	// only handle to destroy. It is NULL already when the window took it.
	if (win->menu) DestroyMenu((HMENU)win->menu);
	if (win->accel) DestroyAcceleratorTable((HACCEL)win->accel);
	win->menu  = NULL;
	win->accel = NULL;
}


void Gui_Menu_Enable(GUIWIN *win, REBCNT item_id, REBOOL enabled)
{
	if (!win || !win->menu) return;
	EnableMenuItem((HMENU)win->menu, (UINT)item_id,
	               MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
	if (win->handle) DrawMenuBar(HWND_OF(win));
}


//== transparency =============================================================
//
// A control with no background of its own has to show what is behind it,
// and on Win32 nothing puts it there. WS_CLIPCHILDREN - which the window
// and the panel both need, so that a parent cannot paint over the controls
// it holds - is exactly what stops the parent painting UNDER them too.
//
// So the control does it itself: its WM_ERASEBKGND asks the parent to
// render its own client area into the control's DC, with the origin
// shifted so that the part which lands inside the control is the part the
// control covers. Every class that can hold a widget answers
// WM_PRINTCLIENT for this - the window with its background, a panel with
// its frame and caption, an image widget with its pixels.
//
// This needs no theme API and no extra library: the possible parents are
// all our own classes.

/***********************************************************************
**  Paints what is behind `hwnd` into `dc`.
***********************************************************************/
static void Paint_Parent_Background(HWND hwnd, HDC dc)
{
	HWND  parent = GetParent(hwnd);
	POINT origin;
	int   saved;

	if (!parent || !dc) return;

	// Where this control's top-left sits in the parent's client area.
	origin.x = 0;
	origin.y = 0;
	MapWindowPoints(hwnd, parent, &origin, 1);

	saved = SaveDC(dc);
	// The parent draws in ITS coordinates; this makes those land in ours.
	OffsetWindowOrgEx(dc, origin.x, origin.y, NULL);
	SendMessageW(parent, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
	RestoreDC(dc, saved);
}


// The original procedures of the system classes a transparent widget can
// be. One per class rather than one per control: every STATIC shares a
// procedure, and so does every BUTTON, so there is nothing per-widget to
// keep and nothing to unwind when a widget goes away.
/***********************************************************************
**  Keyboard handling, in the CONTROL rather than in the pump.
**
**  The keyboard never reaches this extension's message loop. The host
**  drains and dispatches the OS queue itself - Query_Events in
**  dev-event.c - so a WM_KEYDOWN is translated and delivered straight
**  to the focused control, and anything this extension wanted to do
**  with it beforehand simply never runs. That is why the menu
**  accelerators had never worked, and why Enter and Tab did nothing
**  when they were handled in Gui_Pump.
**
**  So every control is subclassed, and the two things that need a
**  keystroke before the control sees it happen here:
**
**    * the menu accelerators, via TranslateAccelerator;
**    * Tab, Shift-Tab, the arrow keys within a group and Space, via
**      IsDialogMessage - the dialog manager is what makes WS_TABSTOP
**      and WS_GROUP mean anything, and it is perfectly happy to be
**      handed a message built here rather than taken from a queue.
**
**  TWO KEYS ARE KEPT BACK from IsDialogMessage, both because it
**  answers them by sending the window a WM_COMMAND with IDOK or
**  IDCANCEL and no control - the exact shape of a menu pick here, so
**  Escape would fire whichever menu item happens to be item 2:
**
**    ENTER  belongs to the focused field, which reports it as a click.
**    ESCAPE is left alone until it means something.
***********************************************************************/
static REBOOL In_Dialog_Message = FALSE;

static REBOOL Gui_Handle_Key(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	HWND    root = GetAncestor(hwnd, GA_ROOT);
	GUIWIN *win  = Our_Window(root);
	MSG     m;
	REBOOL  taken;

	if (!win || !win->handle) return FALSE;
	if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_SYSCHAR)
		return FALSE;

	m.hwnd    = hwnd;
	m.message = msg;
	m.wParam  = wp;
	m.lParam  = lp;
	m.time    = GetMessageTime();
	m.pt.x    = 0;
	m.pt.y    = 0;

	if (win->accel && TranslateAcceleratorW(root, (HACCEL)win->accel, &m))
		return TRUE;

	if (wp == VK_RETURN || wp == VK_ESCAPE) return FALSE;

	/*******************************************************************
	**  TAB is done here rather than by the dialog manager.
	**
	**  IsDialogMessage asks the focused control what it wants, and a
	**  multi-line EDIT answers DLGC_WANTALLKEYS - so Tab in an `area`
	**  would be handed back to the control instead of navigating, and
	**  the user would be trapped in the text box. GetNextDlgTabItem
	**  honours WS_TABSTOP and descends through WS_EX_CONTROLPARENT just
	**  as the dialog manager does, without asking that question.
	*******************************************************************/
	if (msg == WM_KEYDOWN && wp == VK_TAB) {
		HWND next = GetNextDlgTabItem(root, GetFocus(),
			(GetKeyState(VK_SHIFT) & 0x8000) ? TRUE : FALSE);
		if (next && next != GetFocus()) {
			SetFocus(next);
			return TRUE;
		}
		return FALSE;
	}

	/*******************************************************************
	**  Everything else - the arrows within a group, Space, mnemonics -
	**  is the dialog manager's, and it is re-entrant.
	**
	**  When the focused control claims the key (DLGC_WANTALLKEYS, which
	**  is what an `area` answers), IsDialogMessage does not navigate: it
	**  SENDS THE SAME MESSAGE BACK to the control. That arrives in
	**  Nav_Proc again, identical, and without this flag it would call
	**  IsDialogMessage again - down to a stack overflow. The flag turns
	**  the second visit into "not mine", which hands the key to the
	**  control's own procedure, which is exactly what it asked for.
	*******************************************************************/
	if (In_Dialog_Message) return FALSE;

	In_Dialog_Message = TRUE;
	taken = IsDialogMessageW(root, &m) ? TRUE : FALSE;
	In_Dialog_Message = FALSE;

	return taken;
}


/***********************************************************************
**  The procedure every control is given.
**
**  The one it replaces is kept per WIDGET rather than per class: the
**  controls here come from six different classes, and a widget has a
**  place to put it.
***********************************************************************/
/***********************************************************************
**  `down` and `up` on the pressable controls.
**
**  Button, check, radio and slider report the left button going down
**  and coming up on them, in window client coordinates like every mouse
**  event - so a program can tell when the user starts and stops working
**  a slider, or show something only while a button is held.
**
**  Each of these captures the mouse while pressed, so the `up` arrives
**  here wherever the pointer is let go. The order is down, (move and
**  change...), up, (click): `up` goes out BEFORE the base procedure for
**  the button family, which raises BN_CLICKED from inside WM_LBUTTONUP, and AFTER
**  it for a trackbar, which may still post a final position there - so
**  `up` is the last thing a drag reports.
**
**  Capture can also be taken away without a button-up (a window
**  coming forward, Alt+Tab). WM_CAPTURECHANGED then closes the press,
**  so every `down` is matched by exactly one `up`.
***********************************************************************/
static HWND   Pressed_Control = NULL;
static REBOOL In_Button_Up    = FALSE;

static REBOOL Is_Pressable(GUIWIDGET *wid)
{
	if (!wid) return FALSE;
	switch (wid->kind) {
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
	case W_GUI_WIDGET_TOGGLE:
	case W_GUI_WIDGET_SLIDER:
		return TRUE;
	}
	return FALSE;
}

// Closes a press with the pointer's current position, for the paths
// where no button-up message carries one.
static void Release_Press(HWND hwnd, GUIWIDGET *wid)
{
	POINT p;
	Pressed_Control = NULL;
	if (!wid || !wid->hob) return;
	GetCursorPos(&p);
	// Window client coordinates, like every other mouse event.
	ScreenToClient((wid->owner && wid->owner->handle) ? HWND_OF(wid->owner) : hwnd, &p);
	Gui_Queue_Event(wid->hob, EVT_UP, To_Logical(Dpi_Of(hwnd), p.x),
	                To_Logical(Dpi_Of(hwnd), p.y), Modifiers());
}

static LRESULT CALLBACK Nav_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	GUIWIDGET *wid  = (GUIWIDGET*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	WNDPROC    base = (wid && wid->wndproc) ? (WNDPROC)wid->wndproc : NULL;

	/*******************************************************************
	**  A text-list with `scrollable?` off.
	**
	**  The list box keeps its own copy of the style it was created
	**  with, and calls SetScrollInfo whenever its items or its size
	**  change - which puts WS_VSCROLL back on the window however often
	**  it is taken off. So the bit is left alone, and hidden only from
	**  the three non-client messages that act on it: without it, the
	**  frame is calculated, painted and hit-tested as if there were no
	**  scroll bar, and the client area takes its room. Nothing is
	**  recreated, and the list box goes on scrolling itself for
	**  LB_SETTOPINDEX and for a pick made with the keyboard.
	**
	**  The wheel goes to DefWindowProc rather than the list box, which
	**  hands it on to the parent - as for any control that does not
	**  scroll.
	*******************************************************************/
	if (wid && wid->kind == W_GUI_WIDGET_TEXT_LIST && (wid->state & GUI_LIST_FIXED)) {
		LONG_PTR style;
		LRESULT  r;

		if (msg == WM_MOUSEWHEEL) return DefWindowProcW(hwnd, msg, wp, lp);
		if (msg == WM_NCCALCSIZE || msg == WM_NCPAINT || msg == WM_NCHITTEST) {
			style = GetWindowLongPtrW(hwnd, GWL_STYLE);
			if (style & WS_VSCROLL) {
				SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_VSCROLL);
				r = Nav_Proc(hwnd, msg, wp, lp); // the bit is off: no loop
				SetWindowLongPtrW(hwnd, GWL_STYLE,
					GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_VSCROLL);
				return r;
			}
		}
	}

	if (Is_Pressable(wid)) switch (msg) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		// A press left open by a lost message is closed first, so the
		// pairing survives it.
		if (Pressed_Control && Pressed_Control != hwnd) {
			HWND old = Pressed_Control;
			Release_Press(old, (GUIWIDGET*)GetWindowLongPtrW(old, GWLP_USERDATA));
		}
		Pressed_Control = hwnd;
		Queue_Widget_Mouse(wid, EVT_DOWN, lp,
		                   msg == WM_LBUTTONDBLCLK ? GUI_FLAG_DOUBLE : 0);
		break;  // on to the control, which captures and presses

	case WM_LBUTTONUP: {
		LRESULT r;
		REBOOL  open = (Pressed_Control == hwnd);

		if (!open) break;
		if (wid->kind != W_GUI_WIDGET_SLIDER) {
			Pressed_Control = NULL;
			Queue_Widget_Mouse(wid, EVT_UP, lp, 0);
			break;
		}
		In_Button_Up = TRUE;  // the release below is ours to report
		r = base ? CallWindowProcW(base, hwnd, msg, wp, lp)
		         : DefWindowProcW(hwnd, msg, wp, lp);
		In_Button_Up = FALSE;
		if (Pressed_Control == hwnd) {
			Pressed_Control = NULL;
			Queue_Widget_Mouse(wid, EVT_UP, lp, 0);
		}
		return r; }

	case WM_CAPTURECHANGED:
		if (Pressed_Control == hwnd && !In_Button_Up)
			Release_Press(hwnd, wid);
		break;
	}

	// ENTER in a one-line field. An EDIT hands it to the default pushbutton
	// of the dialog it is in; there is none here, so DefWindowProc answers
	// with MessageBeep. Reported as a `click` - the word a button already
	// uses for the same thing - and swallowed, key AND character, because
	// the beep comes from the WM_CHAR.
	//
	// An `area` is multi-line and keeps Enter for itself: it is how a new
	// line is typed.
	if (wid && wid->kind == W_GUI_WIDGET_FIELD && wp == VK_RETURN
	    && (msg == WM_KEYDOWN || msg == WM_CHAR)) {
		if (msg == WM_KEYDOWN && wid->hob) {
			REBINT x = 0, y = 0, w = 0, h = 0;
			Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
			Gui_Queue_Event(wid->hob, EVT_CLICK, x, y, Modifiers());
		}
		return 0;
	}

	// A control covers its part of the window, so the window stops hearing
	// about the mouse there. Every control reports the move instead - a
	// `move` over a button is still a move over the window, and a program
	// following the pointer should not see it vanish over each widget.
	// (A label answers HTTRANSPARENT, so its container reports it.) While
	// a control is pressed it holds the capture, so its moves keep coming
	// wherever the pointer goes - which is what lets a program drag the
	// control, or anything else, with it.
	if (wid) Tip_Relay(hwnd, msg, wp, lp);

	// A progress bar in a dark window is painted whole - see
	// Dark_Progress_Paint(). WM_PRINTCLIENT too, for a transparent parent.
	if (wid && wid->kind == W_GUI_WIDGET_PROGRESS && Dark_For(wid->owner)) {
		if (msg == WM_PAINT) {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			Dark_Progress_Paint(hwnd, dc);
			EndPaint(hwnd, &ps);
			return 0;
		}
		if (msg == WM_PRINTCLIENT) { Dark_Progress_Paint(hwnd, (HDC)wp); return 0; }
		if (msg == WM_ERASEBKGND) return TRUE;
	}

	// A field's or an area's edge in a dark window - see Paint_Dark_Edge().
	if (msg == WM_NCPAINT && wid
	    && (wid->kind == W_GUI_WIDGET_FIELD || wid->kind == W_GUI_WIDGET_AREA
	        || wid->kind == W_GUI_WIDGET_TEXT_LIST)
	    && Dark_For(wid->owner)) {
		LRESULT r = base ? CallWindowProcW(base, hwnd, msg, wp, lp)
		                 : DefWindowProcW(hwnd, msg, wp, lp);
		Paint_Dark_Edge(hwnd);
		return r;
	}

	if (msg == WM_MOUSEMOVE && wid) {
		Track_Leave(hwnd);
		Queue_Widget_Mouse(wid, EVT_MOVE, lp, 0);
	}
	// Handed on afterwards as well: a themed control tracks the pointer
	// itself, for its hot look, and needs the same message.
	if (msg == WM_MOUSELEAVE && wid) Mouse_Left(hwnd);

	if (Gui_Handle_Key(hwnd, msg, wp, lp)) return 0;

	return base ? CallWindowProcW(base, hwnd, msg, wp, lp)
	            : DefWindowProcW(hwnd, msg, wp, lp);
}


// Installed on every control, right after it is created.
static void Subclass_For_Nav(GUIWIDGET *wid)
{
	HWND hwnd;

	if (!wid || !wid->handle || wid->wndproc) return;
	hwnd = HWND_OF_WID(wid);

	wid->wndproc = (void*)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
	SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)Nav_Proc);

	// Every control passes through here once, right after it is created,
	// which makes it the place to give it its window's current look.
	if (Dark_For(wid->owner)) Theme_Control(wid, TRUE);
}


static WNDPROC Static_Proc = NULL;
static WNDPROC Button_Proc = NULL;

/***********************************************************************
**  Paints a transparent control over pixels which are not a colour.
**
**  WM_PAINT rather than WM_ERASEBKGND, and the whole cycle taken over
**  rather than added to. A themed check or radio cross-fades between
**  states through BufferedPaintAnimation: it paints into a memory DC of
**  its own and never asks anyone to erase, so an erase hook is simply
**  not called during the fade and the control animates out of an empty
**  buffer.
**
**  Doing the compositing here - the parent's pixels, then the control
**  over them through WM_PRINTCLIENT - means the base procedure never
**  runs its own WM_PAINT, so there is no animation to go wrong. A
**  transparent control on a picture does not cross-fade, which is a
**  small price and the only way to be sure of what is behind it.
***********************************************************************/
static LRESULT CALLBACK Transparent_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	GUIWIDGET *wid = (GUIWIDGET*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	WNDPROC    base = (wid && wid->kind == W_GUI_WIDGET_TEXT)
	                ? Static_Proc : Button_Proc;

	if (!base) return DefWindowProcW(hwnd, msg, wp, lp);

	// Only while it IS transparent, and only while what is behind it is
	// not a flat colour - otherwise the ordinary path is better in every
	// way. Turning either off leaves the subclass in place and stops
	// using it, which is safer than unhooking a procedure that may be on
	// the stack.
	if (wid && GUI_BG_IS_CLEAR(wid->background)) {
		COLORREF unused;

		if (msg == WM_ERASEBKGND && !Flat_Background_Of(wid, &unused)) {
			Paint_Parent_Background(hwnd, (HDC)wp);
			return TRUE;
		}

		if (msg == WM_PAINT && !Flat_Background_Of(wid, &unused)) {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			Paint_Parent_Background(hwnd, dc);
			// Statics and buttons both render themselves on request,
			// which is what makes this compositing rather than a
			// reimplementation of either.
			CallWindowProcW(base, hwnd, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
			EndPaint(hwnd, &ps);
			return 0;
		}
	}

	return CallWindowProcW(base, hwnd, msg, wp, lp);
}


// Installed the first time a widget is made transparent, never removed.
// A static and a button are different classes, so the procedure being
// replaced is remembered per class.
static void Subclass_For_Transparency(GUIWIDGET *wid)
{
	HWND    hwnd;
	WNDPROC previous;

	if (!wid || !wid->handle) return;
	hwnd = HWND_OF_WID(wid);

	// Chained ON TOP of Nav_Proc, which every control already has: the
	// saved procedure is whatever was there, and Nav_Proc passes anything
	// it does not want down to the control's own.
	previous = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
	if (previous == Transparent_Proc) return; // already done

	if (wid->kind == W_GUI_WIDGET_TEXT) {
		if (!Static_Proc) Static_Proc = previous;
	} else {
		if (!Button_Proc) Button_Proc = previous;
	}
	SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)Transparent_Proc);
}


void Gui_Widget_Set_Background(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;

	// A check and a radio are BUTTONs, a label is a STATIC; the entries
	// and the drop-down have a frame and a background of their own which
	// showing through would only break, so they keep the colour and
	// ignore transparency.
	//
	// And only when what is behind is NOT a flat colour: a transparent
	// widget over the window or over a coloured panel is served by the
	// brush WM_CTLCOLOR* hands back, which keeps the themed animation and
	// needs no subclass at all.
	if (GUI_BG_IS_CLEAR(wid->background)
	    && (wid->kind == W_GUI_WIDGET_TEXT
	     || wid->kind == W_GUI_WIDGET_CHECK
	     || wid->kind == W_GUI_WIDGET_RADIO)) {
		COLORREF flat;
		if (!Flat_Background_Of(wid, &flat)) Subclass_For_Transparency(wid);
	}

	// A CONTAINER's background is also the background its transparent
	// children show, and WS_CLIPCHILDREN means invalidating it does not
	// reach them - so they are taken in explicitly, or they would keep
	// painting the colour the panel used to have.
	if (Kind_Is_Container(wid->kind)) {
		Repaint_Widget(wid, FALSE);
		return;
	}

	// TRUE erases: the old background has to go, and for a transparent
	// widget erasing is what fetches the parent's pixels.
	InvalidateRect(HWND_OF_WID(wid), NULL, TRUE);
}


//== widgets ==================================================================

// What a new control attaches to: the container holding it - a panel or an
// image widget - or the window. `wid->parent` is set before any creation
// call, and the kinds allowed there are decided in the shared layer.
static HWND Parent_Hwnd(GUIWIDGET *wid, GUIWIN *owner)
{
	if (wid->parent && ((GUIWIDGET*)wid->parent)->handle)
		return (HWND)((GUIWIDGET*)wid->parent)->handle;
	return HWND_OF(owner);
}


/***********************************************************************
**  Whether this control starts a new keyboard GROUP.
**
**  WS_GROUP marks the first control of a run, and the arrow keys move
**  within a run - which is the whole of what the dialog manager knows
**  about grouping. Radio grouping HERE is the extension's own: an id
**  passed to add-radio, deliberately independent of creation order.
**
**  The two are reconciled by making every control start its own group
**  EXCEPT a radio whose immediately preceding sibling is a radio of the
**  same id. A run of radios in one group is then one keyboard group, and
**  nothing else arrow-navigates at all - so the arrows cannot walk out
**  of a group and check a radio that belongs to another one.
**
**  The widget is not on the window's list yet - Attach_Widget runs after
**  creation - so the head of that list is the previous sibling.
***********************************************************************/
static REBOOL Starts_New_Group(GUIWIDGET *wid, GUIWIN *owner)
{
	GUIWIDGET *prev;

	if (!wid || !owner) return TRUE;
	if (wid->kind != W_GUI_WIDGET_RADIO) return TRUE;

	// The list is window-wide, so the previous SIBLING is the first entry
	// with the same container.
	for (prev = (GUIWIDGET*)owner->widgets; prev; prev = (GUIWIDGET*)prev->next) {
		if (prev->parent != wid->parent) continue;
		return (prev->kind == W_GUI_WIDGET_RADIO && prev->group == wid->group)
			? FALSE : TRUE;
	}
	return TRUE;
}


REBOOL Gui_Create_Panel(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h,
                        const REBYTE *text, REBCNT len)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;
	// The caller's coordinates are logical units, converted at the DPI of
	// the window the control goes into - which it will share.
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);
	if (!Register_Panel_Class()) return FALSE;

	// WS_CLIPCHILDREN keeps the panel from painting over what it holds.
	//
	// This is deliberately NOT a BS_GROUPBOX button, which is how Win32
	// usually draws a captioned frame: a group box is not a container here
	// but a sibling drawn behind other controls, and parenting children to
	// one is where the classic repaint and tab-order trouble comes from.
	// The panel keeps being a real container and draws the frame itself,
	// which is also what lets the frame be turned on and off later.
	// WS_EX_CONTROLPARENT is what lets keyboard navigation DESCEND into a
	// container. Without it the radios in a titled panel are skipped
	// entirely, because the dialog manager never looks inside.
	hwnd = CreateWindowExW(
		WS_EX_CONTROLPARENT, Class_Name_Panel, L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_GROUP,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance, NULL
	);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);

	// The caption lives in the panel's window text, so the generic text
	// accessor reaches it with no special case of its own.
	// No repaint here: Attach_Widget() redraws every new widget, and it is
	// the one place that decides so.
	if (text && len > 0) Set_Text_Of(hwnd, text, len);

	return TRUE;
}


void Gui_Panel_Edge_Changed(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;
	// TRUE erases first: an edge which has just been turned off has to have
	// its own pixels painted over, not merely stop being drawn.
	InvalidateRect(HWND_OF_WID(wid), NULL, TRUE);
}


REBOOL Gui_Create_Button_Control(GUIWIDGET *wid, GUIWIN *owner,
                                 REBINT x, REBINT y, REBINT w, REBINT h,
                                 const REBYTE *text, REBCNT len)
{
	HWND   hwnd;
	WCHAR *wide;
	DWORD  style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;

	if (!wid || !owner || !owner->handle) return FALSE;
	if (Starts_New_Group(wid, owner)) style |= WS_GROUP;
	// The caller's coordinates are logical units, converted at the DPI of
	// the window the control goes into - which it will share.
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);

	switch (wid->kind) {
	case W_GUI_WIDGET_CHECK:
		// AUTO: the control ticks itself and we read the result back.
		style |= BS_AUTOCHECKBOX;
		break;
	case W_GUI_WIDGET_TOGGLE:
		// A check drawn as a push button: AUTO, so it keeps itself
		// pushed or not, and PUSHLIKE for the look.
		style |= BS_AUTOCHECKBOX | BS_PUSHLIKE;
		break;
	case W_GUI_WIDGET_RADIO:
		// NOT auto: BS_AUTORADIOBUTTON would group by sibling order and
		// WS_GROUP flags, which is not the grouping we promise. This one
		// only reports the click; the state is set from Gui_Widget_Set_State.
		style |= BS_RADIOBUTTON;
		break;
	default:
		style |= BS_PUSHBUTTON;
		break;
	}

	wide = To_Wide(text, len);
	hwnd = CreateWindowExW(
		0,
		L"BUTTON",
		wide ? wide : L"",
		style,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL, // no control id - BN_CLICKED carries the HWND in lParam
		App_Instance, NULL
	);
	if (wide) FREE_MEM(wide);
	if (!hwnd) return FALSE;

	// How the parent's WM_COMMAND finds its way back to the Rebol handle.
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Default_Font_At(Dpi_Of(hwnd)), TRUE);

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);

	return TRUE;
}


/***********************************************************************
**  The static label and the two edit controls.
**
**  All three are stock Win32 classes differing only in style bits, so
**  `wid->kind` picks the class and the flags and the rest is shared.
***********************************************************************/
REBOOL Gui_Create_Text_Control(GUIWIDGET *wid, GUIWIN *owner,
                               REBINT x, REBINT y, REBINT w, REBINT h,
                               const REBYTE *text, REBCNT len)
{
	HWND   hwnd;
	WCHAR *wide;
	const WCHAR *class_name;
	DWORD  style   = WS_CHILD | WS_VISIBLE | WS_GROUP;
	DWORD  exstyle = 0;

	if (!wid || !owner || !owner->handle) return FALSE;
	// The caller's coordinates are logical units, converted at the DPI of
	// the window the control goes into - which it will share.
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);

	switch (wid->kind) {
	case W_GUI_WIDGET_TEXT:
		class_name = L"STATIC";
		style |= SS_LEFT;
		break;

	case W_GUI_WIDGET_FIELD:
		class_name = L"EDIT";
		style   |= WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL;
		exstyle |= WS_EX_CLIENTEDGE;
		break;

	case W_GUI_WIDGET_AREA:
		class_name = L"EDIT";
		style   |= WS_TABSTOP | ES_LEFT | ES_MULTILINE
		         | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL;
		exstyle |= WS_EX_CLIENTEDGE;
		break;

	default:
		return FALSE;
	}

	wide = To_Wide(text, len);
	hwnd = CreateWindowExW(
		exstyle,
		class_name,
		wide ? wide : L"",
		style,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL, // notifications carry the child HWND in lParam
		App_Instance, NULL
	);
	if (wide) FREE_MEM(wide);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Default_Font_At(Dpi_Of(hwnd)), TRUE);

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);

	return TRUE;
}


REBOOL Gui_Create_Image(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;
	// The caller's coordinates are logical units, converted at the DPI of
	// the window the control goes into - which it will share.
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);
	if (!Register_Image_Class()) return FALSE;

	hwnd = CreateWindowExW(
		WS_EX_CONTROLPARENT,   // an image widget is a container too
		Class_Name_Image,
		L"",
		// WS_CLIPCHILDREN for the same reason a panel has it: an image
		// widget can hold other widgets now, and its blit covers every
		// pixel of its client area - including theirs, if not clipped out.
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_GROUP,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance,
		wid // arrives as lpCreateParams in WM_NCCREATE
	);
	if (!hwnd) return FALSE;

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);
	return TRUE;
}


/***********************************************************************
**  A container's children have to be named, and this is why.
**
**  WS_CLIPCHILDREN keeps a container's own repaint out of the rectangles
**  its children occupy - which is what stops it painting over them. But
**  a TRANSPARENT child composites what is behind it as it paints, so
**  when an image widget's pixels change, a caption on top of it is
**  showing pixels which no longer exist and is not repainted by the
**  image's own invalidation. It keeps the picture it was last painted
**  over.
**
**  So a repaint of a container takes RDW_ALLCHILDREN. For a leaf widget
**  it would be pointless - it has no children - and the plain
**  invalidation is left alone there.
***********************************************************************/
static void Repaint_Widget(GUIWIDGET *wid, REBOOL now)
{
	UINT flags = RDW_INVALIDATE;

	if (!wid || !wid->handle) return;

	if (Kind_Is_Container(wid->kind))
		flags |= RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN;
	if (now)
		flags |= RDW_UPDATENOW;

	RedrawWindow(HWND_OF_WID(wid), NULL, NULL, flags);
}


void Gui_Widget_Redraw(GUIWIDGET *wid)
{
	// Painted now rather than whenever the queue next runs dry, so that
	// `redraw` means the pixels are on screen when it returns.
	Repaint_Widget(wid, TRUE);
}


void Gui_Widget_Invalidate(GUIWIDGET *wid)
{
	// Not now: the WM_PAINT this leaves behind is collected by the next
	// pump, along with every other widget invalidated since. See the note
	// in gui.h.
	Repaint_Widget(wid, FALSE);
}


void Gui_Window_Redraw(GUIWIN *win)
{
	if (!win || !win->handle) return;
	InvalidateRect(HWND_OF(win), NULL, TRUE); // children included
	UpdateWindow(HWND_OF(win));
}


void Gui_Destroy_Widget(GUIWIDGET *wid)
{
	// A widget whose window is already gone has a NULL handle: the OS
	// destroyed the control together with its parent.
	HWND tip;
	if (!wid || !wid->handle) return;
	// Its tool first: the tooltip would otherwise keep a stale HWND, which
	// Windows is free to hand to the next control.
	if ((tip = Tip_Of(HWND_OF_WID(wid), FALSE)) != NULL) {
		TOOLINFOW ti;
		Tip_Tool(&ti, wid);
		SendMessageW(tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
	}
	DestroyWindow(HWND_OF_WID(wid));
}


REBOOL Gui_Widget_Set_Tip(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	TOOLINFOW ti, probe;
	HWND      hwnd, tip;
	WCHAR    *wide;
	REBOOL    ok;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	tip = Tip_Of(hwnd, (utf8 && len > 0) ? TRUE : FALSE);
	if (!tip) return (utf8 && len > 0) ? FALSE : TRUE;  // nothing to remove

	Tip_Tool(&ti, wid);
	probe = ti;   // lpszText NULL: asks whether it exists, copies nothing
	if (!utf8 || len == 0) {
		SendMessageW(tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
		return TRUE;
	}

	wide = To_Wide(utf8, len);
	if (!wide) return FALSE;
	ti.lpszText = wide;       // the tooltip keeps a copy of its own
	if (SendMessageW(tip, TTM_GETTOOLINFOW, 0, (LPARAM)&probe)) {
		SendMessageW(tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
		ok = TRUE;
	} else {
		ok = SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti) ? TRUE : FALSE;
	}
	FREE_MEM(wide);
	return ok;
}


REBSER* Gui_Widget_Get_Tip(GUIWIDGET *wid)
{
	TOOLINFOW ti;
	HWND      tip;
	WCHAR     buf[1024];

	if (!wid || !wid->handle) return NULL;
	if (!(tip = Tip_Of(HWND_OF_WID(wid), FALSE))) return NULL;

	Tip_Tool(&ti, wid);
	if (!SendMessageW(tip, TTM_GETTOOLINFOW, 0, (LPARAM)&ti)) return NULL;

	buf[0] = 0;
	Tip_Tool(&ti, wid);
	ti.lpszText = buf;
	SendMessageW(tip, TTM_GETTEXTW, (WPARAM)(sizeof(buf) / sizeof(buf[0])), (LPARAM)&ti);
	buf[(sizeof(buf) / sizeof(buf[0])) - 1] = 0;
	if (!buf[0]) return NULL;
	return RL_ENCODE_UTF8_STRING(buf, (REBCNT)wcslen(buf), TRUE, 0);
}


REBSER* Gui_Widget_Get_Text(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;
	return Text_Of(HWND_OF_WID(wid));
}


REBOOL Gui_Widget_Set_Text(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	if (!wid || !wid->handle) return FALSE;
	return Set_Text_Of(HWND_OF_WID(wid), utf8, len);
}


//== typography ===============================================================

REBOOL Gui_Widget_Get_Font(GUIWIDGET *wid, REBSER **name, REBINT *size,
                           REBCNT *style)
{
	HWND     hwnd;
	HFONT    font;
	LOGFONTW lf;

	if (name)  *name  = NULL;
	if (size)  *size  = 0;
	if (style) *style = 0;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	// WM_GETFONT answers NULL for a control still using the system font,
	// which is not this extension's default - so the default is what an
	// unanswered question means here.
	font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
	if (!font) font = Default_Font_At(Dpi_Of(hwnd));
	if (!font || !GetObjectW(font, sizeof(lf), &lf)) return FALSE;

	if (name && lf.lfFaceName[0]) {
		*name = RL_ENCODE_UTF8_STRING(lf.lfFaceName,
			(REBCNT)lstrlenW(lf.lfFaceName), TRUE, 0);
	}
	if (size) {
		// lfHeight is negative for a character height, positive for a cell
		// height; both are pixels, and points is what Rebol asked about.
		int pixels = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
		*size = (REBINT)MulDiv(pixels, 72, Dpi_Of(hwnd));
	}
	if (style) {
		if (lf.lfWeight >= FW_SEMIBOLD) *style |= GUI_FONT_BOLD;
		if (lf.lfItalic)                *style |= GUI_FONT_ITALIC;
	}
	return TRUE;
}


REBOOL Gui_Widget_Set_Font(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len,
                           REBINT size, REBCNT style)
{
	HWND   hwnd;
	HFONT  font;
	WCHAR *wide = NULL;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	if (utf8 && len > 0) wide = To_Wide(utf8, len);
	font = Font_For(wide, (int)size, style, Dpi_Of(hwnd));
	if (wide) FREE_MEM(wide);
	if (!font) return FALSE;

	// A control does not resize itself for a bigger font, so the box a
	// caller laid out is the box it keeps - which is the same promise the
	// panel's frame makes.
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, MAKELPARAM(TRUE, 0));
	InvalidateRect(hwnd, NULL, TRUE);
	return TRUE;
}


REBOOL Gui_Widget_Set_Color(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;

	// Nothing to apply: the colour is read out of the widget context by
	// the parent's WM_CTLCOLOR* handler, at the moment the control is
	// about to paint. All that is needed is to make it paint.
	InvalidateRect(HWND_OF_WID(wid), NULL, TRUE);

	// ... except on a push button, which never asks. Win32 draws a
	// BS_PUSHBUTTON's text itself, in the system colour, and only an
	// owner-drawn button can say otherwise. A toggle is drawn the same way.
	return (wid->kind == W_GUI_WIDGET_BUTTON || wid->kind == W_GUI_WIDGET_TOGGLE)
	     ? FALSE : TRUE;
}


REBDEC Gui_Get_Scale(GUIWIN *win)
{
	// The window's own: the DPI of the monitor it is on, when the process
	// is per-monitor aware, and the system's otherwise.
	return (REBDEC)Dpi_Of((win && win->handle) ? HWND_OF(win) : NULL) / 96.0;
}


//== screens ==================================================================
//
// A display is named by its GDI device name ("\\.\DISPLAY1"), which stays
// the same while the display is connected. An HMONITOR does not: it can be
// replaced whenever the display configuration changes, so it is never kept
// - every question enumerates the monitors again and matches by name.
//
// DESKTOP COORDINATES. A per-monitor aware process sees the desktop in
// physical pixels, and two monitors at different scales have no common
// logical unit - there is no single factor that turns the whole desktop
// into points. So each monitor is made logical ON ITS OWN, and they are
// pinned together at their top-left corners:
//
//   - a monitor's OFFSET is its physical top-left corner, unscaled;
//   - its SIZE, and any point on it measured from that corner, are
//     divided by its own scale.
//
// A point then belongs to exactly one monitor in both directions: a
// logical rectangle is never bigger than the physical one it came from,
// so two monitors can leave a gap between them but can never overlap.
// Within one monitor - which is where a window is placed, centred or
// kept - everything is in the same logical units as the window's own
// size. This is the rule Qt uses for the same reason.
//
// Under system awareness (the fallback) the whole desktop is scaled by
// the one system DPI instead, which IS a single consistent space.

#define MAX_MONITORS 16

typedef struct {
	MONITORINFOEXW info[MAX_MONITORS];
	HMONITOR       mon[MAX_MONITORS];
	int            count;
} MONITOR_LIST;

static BOOL CALLBACK Collect_Monitor(HMONITOR mon, HDC dc, LPRECT rect, LPARAM lp)
{
	MONITOR_LIST *list = (MONITOR_LIST*)lp;
	(void)dc; (void)rect;

	if (list->count >= MAX_MONITORS) return FALSE;
	ZeroMemory(&list->info[list->count], sizeof(MONITORINFOEXW));
	list->info[list->count].cbSize = sizeof(MONITORINFOEXW);
	if (GetMonitorInfoW(mon, (MONITORINFO*)&list->info[list->count])) {
		list->mon[list->count] = mon;
		list->count++;
	}
	return TRUE;
}

static void Monitor_Key(const MONITORINFOEXW *mi, REBYTE *key)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, mi->szDevice, -1,
	                            (char*)key, GUI_SCREEN_KEY, NULL, NULL);
	if (n <= 0) key[0] = 0;
	key[GUI_SCREEN_KEY - 1] = 0;
}

static REBOOL Find_Monitor(const REBYTE *key, MONITORINFOEXW *out, HMONITOR *mon)
{
	MONITOR_LIST list;
	REBYTE       k[GUI_SCREEN_KEY];
	int          n;

	list.count = 0;
	EnumDisplayMonitors(NULL, NULL, Collect_Monitor, (LPARAM)&list);
	for (n = 0; n < list.count; n++) {
		Monitor_Key(&list.info[n], k);
		if (strcmp((const char*)k, (const char*)key) == 0) {
			*out = list.info[n];
			if (mon) *mon = list.mon[n];
			return TRUE;
		}
	}
	return FALSE;
}

// One desktop coordinate, physical to logical, on a monitor whose physical
// top-left corner is `origin` - see the note at the top of this section.
static REBINT Desk_To_Logical(LONG v, LONG origin, int dpi)
{
	if (!Per_Monitor) return To_Logical(Gui_DPI, (REBINT)v);
	return (REBINT)origin + To_Logical(dpi, (REBINT)(v - origin));
}

static LONG Desk_To_Device(REBINT v, LONG origin, int dpi)
{
	if (!Per_Monitor) return (LONG)To_Device(Gui_DPI, v);
	return origin + (LONG)To_Device(dpi, v - (REBINT)origin);
}

static void Screen_To_Logical(LONG px, LONG py, REBINT *x, REBINT *y)
{
	POINT       pt;
	HMONITOR    mon;
	MONITORINFO mi;
	int         dpi;

	pt.x = px; pt.y = py;
	mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
	ZeroMemory(&mi, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!mon || !GetMonitorInfoW(mon, &mi)) {
		*x = To_Logical(Gui_DPI, (REBINT)px);
		*y = To_Logical(Gui_DPI, (REBINT)py);
		return;
	}
	dpi = Dpi_Of_Monitor(mon);
	*x = Desk_To_Logical(px, mi.rcMonitor.left, dpi);
	*y = Desk_To_Logical(py, mi.rcMonitor.top,  dpi);
}

// The other way needs the monitor the LOGICAL point is on, which is not a
// question Windows can answer - so each monitor's logical rectangle is
// worked out and the point looked for in them. A point on none (in a gap,
// or off every edge) goes to the nearest.
static void Logical_To_Screen(REBINT x, REBINT y, LONG *px, LONG *py, HMONITOR *out)
{
	MONITOR_LIST list;
	int          n, best = -1;
	double       best_d = 0;

	list.count = 0;
	EnumDisplayMonitors(NULL, NULL, Collect_Monitor, (LPARAM)&list);

	for (n = 0; n < list.count; n++) {
		RECT  *r   = &list.info[n].rcMonitor;
		int    dpi = Dpi_Of_Monitor(list.mon[n]);
		REBINT lx  = Desk_To_Logical(r->left,  r->left, dpi);
		REBINT ly  = Desk_To_Logical(r->top,   r->top,  dpi);
		REBINT lr  = Desk_To_Logical(r->right, r->left, dpi);
		REBINT lb  = Desk_To_Logical(r->bottom, r->top, dpi);
		double dx  = (x < lx) ? lx - x : (x >= lr ? x - lr + 1 : 0);
		double dy  = (y < ly) ? ly - y : (y >= lb ? y - lb + 1 : 0);
		double d   = dx * dx + dy * dy;
		if (best < 0 || d < best_d) { best = n; best_d = d; }
		if (d == 0) break;
	}

	if (best < 0) {
		*px = (LONG)To_Device(Gui_DPI, x);
		*py = (LONG)To_Device(Gui_DPI, y);
		if (out) *out = NULL;
		return;
	}
	{
		RECT *r   = &list.info[best].rcMonitor;
		int   dpi = Dpi_Of_Monitor(list.mon[best]);
		*px = Desk_To_Device(x, r->left, dpi);
		*py = Desk_To_Device(y, r->top,  dpi);
		if (out) *out = list.mon[best];
	}
}

REBCNT Gui_Screen_Keys(REBYTE (*keys)[GUI_SCREEN_KEY], REBCNT max)
{
	MONITOR_LIST list;
	REBCNT       at = 0;
	int          pass, n;

	list.count = 0;
	EnumDisplayMonitors(NULL, NULL, Collect_Monitor, (LPARAM)&list);

	// Two passes: the primary first, then the rest in the system's order.
	for (pass = 0; pass < 2; pass++) {
		for (n = 0; n < list.count; n++) {
			REBOOL primary = (list.info[n].dwFlags & MONITORINFOF_PRIMARY) != 0;
			if (primary != (pass == 0)) continue;
			if (at < max) Monitor_Key(&list.info[n], keys[at]);
			at++;
		}
	}
	return at;
}

REBOOL Gui_Screen_Info(const REBYTE *key, GUISCREENINFO *info)
{
	MONITORINFOEXW mi;
	HMONITOR       mon = NULL;
	RECT          *m, *k;
	int            dpi;

	if (!key || !info || !Find_Monitor(key, &mi, &mon)) return FALSE;

	dpi = Dpi_Of_Monitor(mon);
	m = &mi.rcMonitor;
	k = &mi.rcWork;
	info->x  = Desk_To_Logical(m->left, m->left, dpi);
	info->y  = Desk_To_Logical(m->top,  m->top,  dpi);
	info->w  = Desk_To_Logical(m->right,  m->left, dpi) - info->x;
	info->h  = Desk_To_Logical(m->bottom, m->top,  dpi) - info->y;
	info->wx = Desk_To_Logical(k->left, m->left, dpi);
	info->wy = Desk_To_Logical(k->top,  m->top,  dpi);
	info->ww = Desk_To_Logical(k->right,  m->left, dpi) - info->wx;
	info->wh = Desk_To_Logical(k->bottom, m->top,  dpi) - info->wy;
	info->scale   = (REBDEC)dpi / 96.0;
	info->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? TRUE : FALSE;
	return TRUE;
}

// What the monitor calls itself, as the display settings list it. Asked of
// the MONITOR under the adapter output rather than of the output itself,
// whose own name is only the device path. Often a model name; on some
// systems only "Generic PnP Monitor". The device name when even that fails.
REBSER* Gui_Screen_Name(const REBYTE *key)
{
	MONITORINFOEXW mi;
	DISPLAY_DEVICEW dd;
	const WCHAR    *name;

	if (!key || !Find_Monitor(key, &mi, NULL)) return NULL;

	ZeroMemory(&dd, sizeof(dd));
	dd.cb = sizeof(dd);
	name = (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0) && dd.DeviceString[0])
	     ? dd.DeviceString : mi.szDevice;

	return RL_ENCODE_UTF8_STRING((void*)name, (REBCNT)wcslen(name), TRUE, 0);
}

// Where the pointer is on the desktop, for `track-mouse`. See gui.h.
REBOOL Gui_Pointer_At(REBYTE *key, REBINT *x, REBINT *y, REBINT *mods, REBOOL *ours)
{
	POINT          p;
	HWND           under;
	HMONITOR       mon;
	MONITORINFOEXW mi;
	int            dpi;

	*ours = FALSE;
	if (!GetCursorPos(&p)) return FALSE;

	// A press in progress belongs to whatever this thread captured it for,
	// which reports the drag itself - wherever the pointer goes.
	if (GetCapture()) {
		*ours = TRUE;
		return TRUE;
	}

	// One of our windows is under it: its own moves cover this. Asked of
	// the root, since the pointer is usually over one of its controls.
	under = WindowFromPoint(p);
	if (under) {
		WCHAR cls[64];
		HWND  root = GetAncestor(under, GA_ROOT);
		if (root && GetClassNameW(root, cls, 64) && lstrcmpW(cls, Class_Name) == 0) {
			*ours = TRUE;
			return TRUE;
		}
	}

	mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
	ZeroMemory(&mi, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!mon || !GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return FALSE;
	Monitor_Key(&mi, key);

	// From the screen's own corner, in its own units - the same answer the
	// screen's offset and size are given in.
	dpi = Dpi_Of_Monitor(mon);
	*x = Desk_To_Logical(p.x, mi.rcMonitor.left, dpi) - Desk_To_Logical(mi.rcMonitor.left, mi.rcMonitor.left, dpi);
	*y = Desk_To_Logical(p.y, mi.rcMonitor.top,  dpi) - Desk_To_Logical(mi.rcMonitor.top,  mi.rcMonitor.top,  dpi);

	// GetKeyState() is this thread's view of the keyboard, which is stale
	// while another program has the focus - so the asynchronous one.
	*mods = 0;
	if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) *mods |= GUI_FLAG_SHIFT;
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000) *mods |= GUI_FLAG_CONTROL;
	if (GetAsyncKeyState(VK_MENU)    & 0x8000) *mods |= GUI_FLAG_ALT;
	return TRUE;
}

REBOOL Gui_Window_Screen(GUIWIN *win, REBYTE *key)
{
	HMONITOR       mon;
	MONITORINFOEXW mi;

	if (!win || !win->handle || !key) return FALSE;
	// The monitor holding most of the window, the same answer Windows
	// itself uses to decide where a maximised window goes.
	mon = MonitorFromWindow(HWND_OF(win), MONITOR_DEFAULTTONULL);
	if (!mon) return FALSE;

	ZeroMemory(&mi, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return FALSE;
	Monitor_Key(&mi, key);
	return key[0] ? TRUE : FALSE;
}


/***********************************************************************
**  What this widget needs for the text it is holding.
**
**  Answered in LOGICAL units, like every other size here, and only for
**  the kinds which have text - the caller has already decided that a
**  slider has no natural anything.
**
**  The padding numbers are the ones the shell's own dialogs use. They
**  are in logical units and scaled on the way out, so they hold at any
**  DPI.
***********************************************************************/
REBOOL Gui_Widget_Natural_Size(GUIWIDGET *wid, REBINT *w, REBINT *h)
{
	HWND    hwnd;
	HDC     dc;
	HFONT   font, old = NULL;
	TEXTMETRICW tm;
	SIZE    text = {0, 0};
	int     len;
	WCHAR  *caption = NULL;
	REBINT  pad_x = 0, pad_y = 0;
	REBINT  lines = 1;
	int     dpi;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	dc = GetDC(hwnd);
	if (!dc) return FALSE;

	font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
	if (!font) font = Default_Font_At(Dpi_Of(hwnd));
	if (font) old = (HFONT)SelectObject(dc, font);

	GetTextMetricsW(dc, &tm);

	len = GetWindowTextLengthW(hwnd);
	if (len > 0) {
		caption = (WCHAR*)MAKE_MEM((len + 1) * sizeof(WCHAR));
		if (caption) {
			len = GetWindowTextW(hwnd, caption, len + 1);
			GetTextExtentPoint32W(dc, caption, len, &text);
			FREE_MEM(caption);
		}
	}

	if (old) SelectObject(dc, old);
	ReleaseDC(hwnd, dc);

	dpi = Dpi_Of(hwnd);
	switch (wid->kind) {
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_TOGGLE:
		pad_x = To_Device(dpi, 24); pad_y = To_Device(dpi, 12);
		break;
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
		// The box or the dot, and the gap after it.
		pad_x = Metric(dpi, SM_CXMENUCHECK) + To_Device(dpi, 8);
		pad_y = To_Device(dpi, 6);
		break;
	case W_GUI_WIDGET_TEXT:
		pad_y = To_Device(dpi, 4);
		break;
	case W_GUI_WIDGET_AREA:
	case W_GUI_WIDGET_TEXT_LIST:
		// One line is not a useful multi-line box; four is the smallest
		// that looks like one. A list gets a few more, as it is read by
		// scanning down it.
		lines = (wid->kind == W_GUI_WIDGET_TEXT_LIST) ? 6 : 4;
		// fall through
	case W_GUI_WIDGET_FIELD:
	case W_GUI_WIDGET_DROP_DOWN:
		// The sunken border, plus the padding the control keeps inside it.
		pad_x = To_Device(dpi, 8);
		pad_y = To_Device(dpi, 8);
		// The border, unless it was taken off with `/flat` or `edge`.
		if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) {
			pad_x += 2 * Metric(dpi, SM_CXEDGE);
			pad_y += 2 * Metric(dpi, SM_CYEDGE);
		}
		// An entry's width should not be the width of whatever happens to
		// be in it - an empty one would come out a few pixels wide. About
		// twenty characters is what a dialog uses when it has no better
		// idea, and the caller can always give a width of its own.
		if (text.cx < 20 * tm.tmAveCharWidth) text.cx = 20 * tm.tmAveCharWidth;
		break;
	default:
		return FALSE; // no text, so no natural size to give
	}

	if (w) *w = To_Logical(dpi, (REBINT)text.cx + pad_x);

	// tmHeight is ascent plus descent and NOTHING else: tmExternalLeading,
	// the gap the font asks for between its lines, is not in it. A line box
	// is the two together, which is what a multi-line control needs - and
	// for a single line it is a pixel or two of slack in the only direction
	// that matters, since a static draws from the top and anything short
	// clips the descenders.
	if (h) *h = To_Logical(dpi, (REBINT)(tm.tmHeight + tm.tmExternalLeading) * lines
	                       + pad_y);
	return TRUE;
}


REBOOL Gui_Widget_Get_Box(GUIWIDGET *wid, REBINT *x, REBINT *y, REBINT *w, REBINT *h)
{
	RECT  r;
	POINT pt;
	HWND  hwnd, parent;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	// A combo box's window rectangle covers the dropped list as well, which
	// is not the box anyone laid out. The closed control is reported instead.
	if (wid->kind == W_GUI_WIDGET_DROP_DOWN) {
		if (!GetClientRect(hwnd, &r)) return FALSE;
		pt.x = 0; pt.y = 0;
		ClientToScreen(hwnd, &pt);
		parent = GetParent(hwnd);
		if (parent) ScreenToClient(parent, &pt);

		*x = pt.x;
		*y = pt.y;
		*w = r.right - r.left;
		*h = (REBINT)SendMessageW(hwnd, CB_GETITEMHEIGHT, (WPARAM)-1, 0)
		   + 2 * Metric(Dpi_Of(hwnd), SM_CYEDGE);
		Box_To_Logical(Dpi_Of(hwnd), x, y, w, h);
		return TRUE;
	}

	if (!GetWindowRect(hwnd, &r)) return FALSE;

	// GetWindowRect is in screen coordinates; the offset is wanted inside
	// the parent's client area.
	pt.x = r.left;
	pt.y = r.top;
	parent = GetParent(hwnd);
	if (parent) ScreenToClient(parent, &pt);

	*x = pt.x;
	*y = pt.y;
	*w = r.right - r.left;
	*h = r.bottom - r.top;
	Box_To_Logical(Dpi_Of(hwnd), x, y, w, h);
	return TRUE;
}


REBOOL Gui_Widget_Set_Box(GUIWIDGET *wid, REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND hwnd, parent;
	RECT before, after, dirty;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);

	Box_To_Device(Dpi_Of(hwnd), &x, &y, &w, &h);
	// ... and the same room has to be added back when it is moved. It is a
	// device-pixel constant, so it is added AFTER the conversion.
	if (wid->kind == W_GUI_WIDGET_DROP_DOWN) h += DROP_LIST_ROOM;

	// Where it is now, in the coordinates the new box is given in - the
	// client area of whatever holds it, a window or a panel.
	parent = GetParent(hwnd);
	if (parent && GetWindowRect(hwnd, &before))
		MapWindowPoints(NULL, parent, (POINT*)&before, 2);
	else
		parent = NULL;

	if (!MoveWindow(hwnd, x, y, w, h, TRUE)) return FALSE;

	/*******************************************************************
	**  A control which moved or shrank leaves its old rectangle behind,
	**  and that rectangle belongs to the PARENT: Windows invalidates it
	**  there, and the parent paints its background over the lot.
	**
	**  Which erases any SIBLING control living in that area - and the
	**  sibling is a window of its own, whose client area Windows still
	**  considers valid, so it is never sent a WM_PAINT and never comes
	**  back. Grow a label over a field, shrink it again, and the field
	**  is left half painted.
	**
	**  RDW_ALLCHILDREN over the union of where the control was and where
	**  it now is takes the siblings in with it. No RDW_UPDATENOW: this
	**  marks, and the pump paints - see Gui_Widget_Invalidate.
	*******************************************************************/
	if (parent) {
		SetRect(&after, x, y, x + w, y + h);
		UnionRect(&dirty, &before, &after);
		// RDW_FRAME as well as RDW_ERASE: a sibling is not clipped out of
		// another sibling, so one can still paint over another's sunken
		// border - and a border lives in the NON-client area, which
		// invalidating the client area alone never repaints.
		RedrawWindow(parent, &dirty, NULL,
		             RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
	}
	Tip_Follow(wid);
	return TRUE;
}


REBOOL Gui_Create_Range_Control(GUIWIDGET *wid, GUIWIN *owner,
                                REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND  hwnd;
	const WCHAR *class_name;
	DWORD style = WS_CHILD | WS_VISIBLE | WS_GROUP;

	if (!wid || !owner || !owner->handle) return FALSE;
	// The caller's coordinates are logical units, converted at the DPI of
	// the window the control goes into - which it will share.
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);

	if (wid->kind == W_GUI_WIDGET_SLIDER) {
		class_name = TRACKBAR_CLASSW;
		style |= WS_TABSTOP | TBS_NOTICKS;
		// Taller than wide means upright - the same rule the old View
		// widgets used, and one less argument to pass.
		if (h > w) style |= TBS_VERT;
	} else {
		class_name = PROGRESS_CLASSW;
	}

	hwnd = CreateWindowExW(
		0, class_name, L"", style,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance, NULL
	);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);

	if (wid->kind == W_GUI_WIDGET_SLIDER) {
		SendMessageW(hwnd, TBM_SETRANGE, (WPARAM)TRUE,
		             (LPARAM)MAKELONG(0, RANGE_STEPS));
		SendMessageW(hwnd, TBM_SETPAGESIZE, 0, (LPARAM)(RANGE_STEPS / 10));
	} else {
		SendMessageW(hwnd, PBM_SETRANGE32, 0, (LPARAM)RANGE_STEPS);
	}

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);
	return TRUE;
}


// A vertical trackbar puts position 0 at the TOP, which is upside down
// compared with what a caller means by 0%. Both directions are flipped
// here so that the extension's 0.0 is always the bottom.
static REBOOL Is_Vertical_Slider(GUIWIDGET *wid)
{
	return (wid->kind == W_GUI_WIDGET_SLIDER
	     && (GetWindowLongPtrW(HWND_OF_WID(wid), GWL_STYLE) & TBS_VERT))
		? TRUE : FALSE;
}


REBDEC Gui_Widget_Get_Value(GUIWIDGET *wid)
{
	LRESULT pos;

	if (!wid || !wid->handle) return 0.0;

	if (wid->kind == W_GUI_WIDGET_SLIDER) {
		pos = SendMessageW(HWND_OF_WID(wid), TBM_GETPOS, 0, 0);
		if (Is_Vertical_Slider(wid)) pos = RANGE_STEPS - pos;
	} else {
		pos = SendMessageW(HWND_OF_WID(wid), PBM_GETPOS, 0, 0);
	}
	return (REBDEC)pos / (REBDEC)RANGE_STEPS;
}


void Gui_Widget_Set_Value(GUIWIDGET *wid, REBDEC value)
{
	LRESULT pos;

	if (!wid || !wid->handle) return;

	pos = (LRESULT)(value * RANGE_STEPS + 0.5);
	if (pos < 0) pos = 0;
	if (pos > RANGE_STEPS) pos = RANGE_STEPS;

	if (wid->kind == W_GUI_WIDGET_SLIDER) {
		if (Is_Vertical_Slider(wid)) pos = RANGE_STEPS - pos;
		SendMessageW(HWND_OF_WID(wid), TBM_SETPOS, (WPARAM)TRUE, (LPARAM)pos);
		return;
	}

	/*******************************************************************
	**  Set as is, so a themed progress bar SLIDES to the new position
	**  over a couple of hundred milliseconds, the way Windows' own do.
	**  That is the control's own animation, and it has two limits: it
	**  plays only when the position increases (a decrease jumps), and
	**  not in a window drawn by `dark-controls?`, which paints the bar
	**  itself at its final position.
	*******************************************************************/
	SendMessageW(HWND_OF_WID(wid), PBM_SETPOS, (WPARAM)pos, 0);
}


//-- drop-down ----------------------------------------------------------------

REBOOL Gui_Create_Drop_Down(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;
	// The caller's coordinates are logical units, converted at the DPI of
	// the window the control goes into - which it will share.
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);

	hwnd = CreateWindowExW(
		0, L"COMBOBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_GROUP
		| CBS_DROPDOWNLIST | CBS_HASSTRINGS,
		x, y, w, h + DROP_LIST_ROOM, // see DROP_LIST_ROOM
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance, NULL
	);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Default_Font_At(Dpi_Of(hwnd)), TRUE);

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);

	return TRUE;
}


//-- text-list ----------------------------------------------------------------

REBOOL Gui_Create_Text_List(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;
	Box_To_Device(Dpi_Of(HWND_OF(owner)), &x, &y, &w, &h);

	// WS_VSCROLL without LBS_DISABLENOSCROLL is a scroll bar that shows
	// only while the items do not fit. LBS_NOINTEGRALHEIGHT keeps the box
	// the size it was given, rather than shrinking it to whole rows.
	hwnd = CreateWindowExW(
		WS_EX_CLIENTEDGE, L"LISTBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_GROUP
		| LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance, NULL
	);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Default_Font_At(Dpi_Of(hwnd)), TRUE);

	wid->handle = (void*)hwnd;
	Subclass_For_Nav(wid);

	return TRUE;
}


/***********************************************************************
**  The items of a drop-down and of a text-list.
**
**  A combo box and a list box hold their strings the same way under
**  different message numbers, and report failure with different values:
**  so each call picks its message by kind, and CB_ERR and LB_ERR are both
**  -1, CB_ERRSPACE and LB_ERRSPACE both -2.
***********************************************************************/
#define IS_LIST(wid) ((wid)->kind == W_GUI_WIDGET_TEXT_LIST)

REBCNT Gui_Widget_Count_Items(GUIWIDGET *wid)
{
	LRESULT count;
	if (!wid || !wid->handle) return 0;
	count = SendMessageW(HWND_OF_WID(wid), IS_LIST(wid) ? LB_GETCOUNT : CB_GETCOUNT, 0, 0);
	return (count < 0) ? 0 : (REBCNT)count;
}


REBSER* Gui_Widget_Get_Item(GUIWIDGET *wid, REBCNT n)
{
	LRESULT len;
	WCHAR  *buf;
	REBSER *str;
	HWND    hwnd;

	if (!wid || !wid->handle) return NULL;
	hwnd = HWND_OF_WID(wid);

	len = SendMessageW(hwnd, IS_LIST(wid) ? LB_GETTEXTLEN : CB_GETLBTEXTLEN, (WPARAM)n, 0);
	if (len < 0) return NULL;
	if (len == 0) return RL_MAKE_STRING(0, FALSE);

	buf = (WCHAR*)MAKE_MEM(((size_t)len + 1) * sizeof(WCHAR));
	if (!buf) return NULL;

	len = SendMessageW(hwnd, IS_LIST(wid) ? LB_GETTEXT : CB_GETLBTEXT, (WPARAM)n, (LPARAM)buf);
	if (len < 0) { FREE_MEM(buf); return NULL; }

	str = RL_ENCODE_UTF8_STRING(buf, (REBCNT)len, TRUE, 0);
	FREE_MEM(buf);
	return str;
}


REBOOL Gui_Widget_Add_Item(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	WCHAR *wide;
	LRESULT res;

	if (!wid || !wid->handle) return FALSE;

	wide = To_Wide(utf8, len);
	res = SendMessageW(HWND_OF_WID(wid), IS_LIST(wid) ? LB_ADDSTRING : CB_ADDSTRING, 0,
	                   (LPARAM)(wide ? wide : L""));
	if (wide) FREE_MEM(wide);
	return (res < 0) ? FALSE : TRUE;
}


void Gui_Widget_Clear_Items(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;
	SendMessageW(HWND_OF_WID(wid), IS_LIST(wid) ? LB_RESETCONTENT : CB_RESETCONTENT, 0, 0);
}


REBINT Gui_Widget_Get_Index(GUIWIDGET *wid)
{
	LRESULT n;
	if (!wid || !wid->handle) return -1;
	n = SendMessageW(HWND_OF_WID(wid), IS_LIST(wid) ? LB_GETCURSEL : CB_GETCURSEL, 0, 0);
	return (n < 0) ? -1 : (REBINT)n;
}


void Gui_Widget_Set_Index(GUIWIDGET *wid, REBINT n)
{
	if (!wid || !wid->handle) return;
	// -1 clears the selection, for both, which is what an index out of
	// range means here. A list box also scrolls the pick into view.
	SendMessageW(HWND_OF_WID(wid), IS_LIST(wid) ? LB_SETCURSEL : CB_SETCURSEL, (WPARAM)n, 0);
}


REBOOL Gui_Widget_Get_State(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	return (SendMessageW(HWND_OF_WID(wid), BM_GETCHECK, 0, 0) == BST_CHECKED)
		? TRUE : FALSE;
}


/***********************************************************************
**  Setting a check or a radio - but only when it is not already there.
**
**  Not an optimisation. With the v6 common controls a check and a radio
**  CROSS-FADE between states, and BM_SETCHECK restarts that animation
**  whether or not the state actually changed. The radio grouping above
**  this file re-asserts every radio in the window on every click, so a
**  redundant write here means every radio on screen begins a fade each
**  time any one of them is picked - which is exactly the sluggishness
**  the modern theme brings and the classic one does not.
**
**  The control is asked rather than trusting wid->state, because the
**  user clicking a checkbox changes the control without going through
**  this extension at all.
***********************************************************************/
void Gui_Widget_Set_State(GUIWIDGET *wid, REBOOL on)
{
	HWND    hwnd;
	LRESULT want, has;

	if (!wid || !wid->handle) return;
	hwnd = HWND_OF_WID(wid);

	want = on ? BST_CHECKED : BST_UNCHECKED;
	has  = SendMessageW(hwnd, BM_GETCHECK, 0, 0);
	if (has == want) return;

	SendMessageW(hwnd, BM_SETCHECK, (WPARAM)want, 0);
}


REBOOL Gui_Widget_Get_Enabled(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	return IsWindowEnabled(HWND_OF_WID(wid)) ? TRUE : FALSE;
}


REBOOL Gui_Widget_Set_Enabled(GUIWIDGET *wid, REBOOL enabled)
{
	if (!wid || !wid->handle) return FALSE;
	EnableWindow(HWND_OF_WID(wid), enabled ? TRUE : FALSE);
	return TRUE;
}


/***********************************************************************
**  Scrolling, through the scrollbar rather than the text.
**
**  GetScrollInfo is what makes this short: an EDIT control's vertical
**  range is already in lines, with nPage the number visible, so the
**  fraction is the standard nPos / (nMax - nMin - nPage + 1) and no
**  font has to be measured to find out how many lines fit.
***********************************************************************/
static REBOOL Scroll_Span_Of(HWND hwnd, SCROLLINFO *si, REBINT *span)
{
	ZeroMemory(si, sizeof(*si));
	si->cbSize = sizeof(*si);
	si->fMask  = SIF_ALL;
	if (!GetScrollInfo(hwnd, SB_VERT, si)) return FALSE;

	*span = si->nMax - si->nMin - (REBINT)si->nPage + 1;
	return TRUE;
}


/***********************************************************************
**  A text-list scrolls by whole rows, and is asked by them - not by its
**  scroll bar, which is not there when `scrollable?` is off. The rows
**  that fit are the client height over the row height, and the span is
**  the rest.
***********************************************************************/
static REBINT List_Span_Of(HWND hwnd)
{
	RECT    r;
	LRESULT count = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
	LRESULT row   = SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0);
	REBINT  fit;

	if (count <= 0 || row <= 0 || !GetClientRect(hwnd, &r)) return 0;
	fit = (REBINT)((r.bottom - r.top) / row);
	if (fit < 1) fit = 1;
	return (REBINT)count - fit;
}


void Gui_Widget_Set_Scrollable(GUIWIDGET *wid, REBOOL on)
{
	if (!wid || !wid->handle) return;
	// Nav_Proc reads the flag; the frame only has to be worked out again,
	// with or without the scroll bar's room. SWP_DRAWFRAME repaints it.
	SetWindowPos(HWND_OF_WID(wid), NULL, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
	             | SWP_FRAMECHANGED | SWP_DRAWFRAME);
	InvalidateRect(HWND_OF_WID(wid), NULL, TRUE);
}


/***********************************************************************
**  The border is WS_EX_CLIENTEDGE: a frame style, so taking it off is
**  SWP_FRAMECHANGED and the window keeps its rectangle - the client area
**  grows into the room. The dark edge (Paint_Dark_Edge) already checks
**  the bit, so a flat control in a dark window gets no ring either.
***********************************************************************/
REBOOL Gui_Widget_Get_Edge(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	return (GetWindowLongPtrW(HWND_OF_WID(wid), GWL_EXSTYLE) & WS_EX_CLIENTEDGE)
		? TRUE : FALSE;
}


void Gui_Widget_Set_Edge(GUIWIDGET *wid, REBOOL on)
{
	HWND     hwnd;
	LONG_PTR ex;

	if (!wid || !wid->handle) return;
	hwnd = HWND_OF_WID(wid);
	ex   = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	ex   = on ? (ex | WS_EX_CLIENTEDGE) : (ex & ~(LONG_PTR)WS_EX_CLIENTEDGE);
	SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
	SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
	             | SWP_FRAMECHANGED | SWP_DRAWFRAME);
	InvalidateRect(hwnd, NULL, TRUE);
}


void Gui_Widget_Scroll_To_Item(GUIWIDGET *wid, REBINT n)
{
	HWND    hwnd;
	RECT    r;
	LRESULT top, row;
	REBINT  fit;

	if (!wid || !wid->handle) return;
	hwnd = HWND_OF_WID(wid);
	top  = SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0);
	row  = SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0);
	if (top < 0 || row <= 0 || !GetClientRect(hwnd, &r)) return;
	fit = (REBINT)((r.bottom - r.top) / row);
	if (fit < 1) fit = 1;

	// Above the view: it becomes the top row. Below: the bottom one.
	if (n < (REBINT)top)
		SendMessageW(hwnd, LB_SETTOPINDEX, (WPARAM)n, 0);
	else if (n >= (REBINT)top + fit)
		SendMessageW(hwnd, LB_SETTOPINDEX, (WPARAM)(n - fit + 1), 0);
}


REBDEC Gui_Widget_Get_Scroll(GUIWIDGET *wid)
{
	SCROLLINFO si;
	REBINT     span = 0;

	if (!wid || !wid->handle) return -1.0;
	if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
		span = List_Span_Of(HWND_OF_WID(wid));
		if (span <= 0) return 0.0;
		return (REBDEC)SendMessageW(HWND_OF_WID(wid), LB_GETTOPINDEX, 0, 0) / (REBDEC)span;
	}
	if (!Scroll_Span_Of(HWND_OF_WID(wid), &si, &span)) return -1.0;

	// Everything fits, so it is at the top and cannot be anywhere else.
	if (span <= 0) return 0.0;
	return (REBDEC)(si.nPos - si.nMin) / (REBDEC)span;
}


REBOOL Gui_Widget_Set_Scroll(GUIWIDGET *wid, REBDEC where)
{
	SCROLLINFO si;
	REBINT     span = 0, target, delta;
	HWND       hwnd;

	if (!wid || !wid->handle) return FALSE;
	hwnd = HWND_OF_WID(wid);
	if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
		span = List_Span_Of(hwnd);
		if (span <= 0) return TRUE;
		// Clamped by the list box itself, so the end is always the end.
		SendMessageW(hwnd, LB_SETTOPINDEX, (WPARAM)(REBINT)((REBDEC)span * where + 0.5), 0);
		return TRUE;
	}
	if (!Scroll_Span_Of(hwnd, &si, &span)) return FALSE;
	if (span <= 0) return TRUE; // nothing to scroll, and that is not a failure

	target = si.nMin + (REBINT)((REBDEC)span * where + 0.5);
	if (target < si.nMin)        target = si.nMin;
	if (target > si.nMin + span) target = si.nMin + span;

	// EM_LINESCROLL takes a DELTA, not a position, and clamps it - which
	// is also why the end is reliable: an over-large delta lands there
	// rather than failing.
	delta = target - si.nPos;
	if (delta) SendMessageW(hwnd, EM_LINESCROLL, 0, (LPARAM)delta);
	return TRUE;
}


// ES_READONLY and WS_DISABLED are separate bits here, so the two compose
// without either being reconstructed from the other - the reason this is
// three lines on Windows and a combination on macOS.
REBOOL Gui_Widget_Set_Read_Only(GUIWIDGET *wid, REBOOL on)
{
	if (!wid || !wid->handle) return FALSE;
	SendMessageW(HWND_OF_WID(wid), EM_SETREADONLY, (WPARAM)(on ? TRUE : FALSE), 0);
	return TRUE;
}