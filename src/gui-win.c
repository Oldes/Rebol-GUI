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

#define WINDOW_STYLE   (WS_OVERLAPPEDWINDOW)
#define WINDOW_EXSTYLE (0)

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
	WCHAR  name[LF_FACESIZE];
} FONTCACHE;

static FONTCACHE *Font_Cache = NULL;

// Points per inch on this display, which is what turns a point size into
// the pixel height a LOGFONT wants.
static int Screen_DPI(void)
{
	HDC dc = GetDC(NULL);
	int dpi = 96;
	if (dc) {
		int y = GetDeviceCaps(dc, LOGPIXELSY);
		if (y > 0) dpi = y;
		ReleaseDC(NULL, dc);
	}
	return dpi;
}

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
static HFONT Font_For(const WCHAR *name, int size, REBCNT style)
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
		lf.lfHeight = -MulDiv(size, Screen_DPI(), 72);
		lf.lfWidth  = 0;
	}
	lf.lfWeight = (style & GUI_FONT_BOLD) ? FW_BOLD : FW_NORMAL;
	lf.lfItalic = (style & GUI_FONT_ITALIC) ? TRUE : FALSE;

	for (entry = Font_Cache; entry; entry = entry->next) {
		if (entry->size == size && entry->style == style
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
	lstrcpynW(entry->name, lf.lfFaceName, LF_FACESIZE);
	entry->next  = Font_Cache;
	Font_Cache   = entry;

	return font;
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
static LRESULT Ctl_Color(HDC dc, HWND child, GUIWIN *win)
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

	SetBkColor(dc, GetSysColor(COLOR_WINDOW));
	SetTextColor(dc, (wid && GUI_COLOR_HAS(wid->color))
		? RGB(GUI_COLOR_R(wid->color),
		      GUI_COLOR_G(wid->color),
		      GUI_COLOR_B(wid->color))
		: GetSysColor(COLOR_WINDOWTEXT));

	return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);
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

// Queues a mouse event at the position carried by lParam.
static void Queue_Mouse(GUIWIN *win, REBCNT type, LPARAM lp, REBINT extra)
{
	if (!win || !win->hob) return;
	Gui_Queue_Event(win->hob, type, GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
	                Modifiers() | extra);
}

// The same for a child widget. A child covers its part of the window, so
// the parent stops hearing about the mouse there - the widget reports it
// instead, with itself as the source and its own coordinates.
static void Queue_Widget_Mouse(GUIWIDGET *wid, REBCNT type, LPARAM lp, REBINT extra)
{
	if (!wid || !wid->hob) return;
	Gui_Queue_Event(wid->hob, type, GET_X_LPARAM(lp), GET_Y_LPARAM(lp),
	                Modifiers() | extra);
}


//== window procedure =========================================================

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

	switch (msg) {

	case WM_MOUSEMOVE:
		Queue_Mouse(win, W_GUI_EVENT_MOVE, lp, 0);
		return 0;

	case WM_LBUTTONDBLCLK:
		Queue_Mouse(win, W_GUI_EVENT_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONDOWN:
		Queue_Mouse(win, W_GUI_EVENT_DOWN, lp, 0);
		SetFocus(hwnd);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONUP:
		Queue_Mouse(win, W_GUI_EVENT_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_RBUTTONDBLCLK:
		Queue_Mouse(win, W_GUI_EVENT_ALT_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_RBUTTONDOWN:
		Queue_Mouse(win, W_GUI_EVENT_ALT_DOWN, lp, 0);
		SetFocus(hwnd);
		SetCapture(hwnd);
		return 0;
	case WM_RBUTTONUP:
		Queue_Mouse(win, W_GUI_EVENT_ALT_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_MBUTTONDBLCLK:
		Queue_Mouse(win, W_GUI_EVENT_AUX_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_MBUTTONDOWN:
		Queue_Mouse(win, W_GUI_EVENT_AUX_DOWN, lp, 0);
		SetFocus(hwnd);
		SetCapture(hwnd);
		return 0;
	case WM_MBUTTONUP:
		Queue_Mouse(win, W_GUI_EVENT_AUX_UP, lp, 0);
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
			Gui_Queue_Event(win->hob, W_GUI_EVENT_WHEEL, pt.x, pt.y,
			                delta * (REBINT)lines);
		return 0; }

	case WM_SIZE:
		if (wp != SIZE_MINIMIZED && win->hob)
			Gui_Queue_Event(win->hob, W_GUI_EVENT_RESIZE,
			                (REBINT)LOWORD(lp), (REBINT)HIWORD(lp), 0);
		return 0;

	case WM_CLOSE:
		// Only reported - closing is Rebol's decision, and doing it here
		// would destroy a window whose handle is still in use.
		if (win->hob)
			Gui_Queue_Event(win->hob, W_GUI_EVENT_CLOSE, 0, 0, 0);
		return 0;

	case WM_COMMAND: {
		// Child controls report through their parent, and the child's HWND
		// arrives in lParam - which is why the control does not need an id.
		HWND child = (HWND)lp;
		GUIWIDGET *wid;
		REBCNT type;
		REBINT x = 0, y = 0, w = 0, h = 0;

		if (!child) break;

		wid = (GUIWIDGET*)GetWindowLongPtrW(child, GWLP_USERDATA);

		// Notification codes overlap between control families (BN_CLICKED
		// and CBN_ERRSPACE are both 0), so the combo box codes are only
		// read when the control really is one.
		if (wid && wid->kind == W_GUI_WIDGET_DROP_DOWN) {
			switch (HIWORD(wp)) {
			case CBN_SELCHANGE: type = W_GUI_EVENT_CHANGE;  break;
			case CBN_SETFOCUS:  type = W_GUI_EVENT_FOCUS;   break;
			case CBN_KILLFOCUS: type = W_GUI_EVENT_UNFOCUS; break;
			default: goto not_handled;
			}
			if (wid->hob) {
				Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
				Gui_Queue_Event(wid->hob, type, x, y, Modifiers());
			}
			return 0;
		}

		switch (HIWORD(wp)) {
		case BN_CLICKED:   type = W_GUI_EVENT_CLICK;   break;
		// SetWindowText raises EN_CHANGE as well, and reporting our own
		// writes back as user edits would turn every `field/text: ...`
		// into an event.
		case EN_CHANGE:    if (Setting_Text) return 0;
		                   type = W_GUI_EVENT_CHANGE;  break;
		case EN_SETFOCUS:  type = W_GUI_EVENT_FOCUS;   break;
		case EN_KILLFOCUS: type = W_GUI_EVENT_UNFOCUS; break;
		default: goto not_handled;
		}

		if (wid && wid->hob) {
			// The position slot carries the widget's own offset - a
			// notification has no cursor position of its own.
			Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
			if (type == W_GUI_EVENT_CLICK) {
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
			Gui_Queue_Event(wid->hob, W_GUI_EVENT_CHANGE, x, y, Modifiers());
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
		return Ctl_Color((HDC)wp, (HWND)lp, win);

	case WM_ERASEBKGND:
		return TRUE; // painted below, without the flicker

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		FillRect(dc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));
		EndPaint(hwnd, &ps);
		return 0; }

	case WM_NCDESTROY:
		// The window is gone for good - whether we destroyed it or the
		// system did. Drop the queued events which point at the handle and
		// release the lock that kept it alive.
		win->handle = NULL;
		win->flags &= ~GUIW_VISIBLE;
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
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

	case WM_ERASEBKGND:
		return TRUE; // WM_PAINT covers every pixel

	case WM_PAINT: {
		PAINTSTRUCT ps;
		RECT   rect;
		HDC    dc;
		REBYTE *bits = NULL;
		REBINT  iw = 0, ih = 0;

		dc = BeginPaint(hwnd, &ps);
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
			FillRect(dc, &rect, (HBRUSH)(COLOR_WINDOW + 1));
		}

		EndPaint(hwnd, &ps);
		return 0; }

	case WM_MOUSEMOVE:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_MOVE, lp, 0);
		return 0;

	case WM_LBUTTONDBLCLK:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_DOWN, lp, GUI_FLAG_DOUBLE);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONDOWN:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_DOWN, lp, 0);
		SetCapture(hwnd);
		return 0;
	case WM_LBUTTONUP:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_UP, lp, 0);
		ReleaseCapture();
		return 0;

	case WM_RBUTTONDOWN:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_ALT_DOWN, lp, 0);
		return 0;
	case WM_RBUTTONUP:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_ALT_UP, lp, 0);
		return 0;

	case WM_MBUTTONDOWN:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_AUX_DOWN, lp, 0);
		return 0;
	case WM_MBUTTONUP:
		Queue_Widget_Mouse(wid, W_GUI_EVENT_AUX_UP, lp, 0);
		return 0;
	}

	return DefWindowProcW(hwnd, msg, wp, lp);
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

// How far in from the left edge a framed panel's caption starts. The same
// number is used on macOS, so the two look alike even though each measures
// the text with its own font.
#define PANEL_CAPTION_X 9

static LRESULT CALLBACK Gui_Panel_Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {

	case WM_COMMAND:
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
		RECT   rect, frame, gap;
		HDC    dc = BeginPaint(hwnd, &ps);
		GUIWIDGET *wid = (GUIWIDGET*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		int    caption_len = GetWindowTextLengthW(hwnd);
		WCHAR *caption = NULL;
		HFONT  font, old_font = NULL;
		SIZE   text_size = {0, 0};
		int    inset = 0;

		GetClientRect(hwnd, &rect);
		// The same background the window paints, so a panel is a place to
		// put things rather than a visible slab.
		FillRect(dc, &rect, (HBRUSH)(COLOR_WINDOW + 1));

		if (!wid || !(wid->state & GUI_PANEL_EDGE)) {
			EndPaint(hwnd, &ps);
			return 0;
		}

		// The caption is kept as the panel's window text, and drawn with
		// the panel's own font - which is what makes `panel/text`,
		// `panel/font-size` and the rest work through the generic
		// accessors, with no special case above this file.
		font = (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0);
		if (!font) font = Get_Default_Font();
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
			gap.left   = PANEL_CAPTION_X - 2;
			gap.top    = rect.top;
			gap.right  = gap.left + text_size.cx + 4;
			gap.bottom = rect.top + text_size.cy;
			if (gap.right > rect.right) gap.right = rect.right;
			FillRect(dc, &gap, (HBRUSH)(COLOR_WINDOW + 1));

			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, GUI_COLOR_HAS(wid->color)
				? RGB(GUI_COLOR_R(wid->color),
				      GUI_COLOR_G(wid->color),
				      GUI_COLOR_B(wid->color))
				: GetSysColor(COLOR_WINDOWTEXT));
			TextOutW(dc, PANEL_CAPTION_X, rect.top, caption, caption_len);
			FREE_MEM(caption);
		}

		if (old_font) SelectObject(dc, old_font);
		EndPaint(hwnd, &ps);
		return 0; }
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

	shcore = LoadLibraryW(L"shcore.dll");
	if (shcore) {
		SETPROCESSDPIAWARENESS_T fn =
			(SETPROCESSDPIAWARENESS_T)GetProcAddress(shcore, "SetProcessDpiAwareness");
		if (fn) {
			fn(1); // PROCESS_SYSTEM_DPI_AWARE
			FreeLibrary(shcore);
			return;
		}
		FreeLibrary(shcore);
	}
	user32 = LoadLibraryW(L"user32.dll");
	if (user32) {
		SETPROCESSDPIAWARE_T fn =
			(SETPROCESSDPIAWARE_T)GetProcAddress(user32, "SetProcessDPIAware");
		if (fn) fn();
		FreeLibrary(user32);
	}
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
                       const REBYTE *title, REBCNT title_len)
{
	HWND  hwnd;
	RECT  rect;
	WCHAR *wide;

	if (!Register_Class()) return FALSE;

	// The requested size is the CLIENT size - grow it by the frame.
	rect.left = 0; rect.top = 0; rect.right = w; rect.bottom = h;
	AdjustWindowRectEx(&rect, WINDOW_STYLE, FALSE, WINDOW_EXSTYLE);

	// A missing - or empty - title gets a neutral default rather than an
	// empty title bar.
	wide = To_Wide(title, title_len);

	hwnd = CreateWindowExW(
		WINDOW_EXSTYLE,
		Class_Name,
		wide ? wide : L"Rebol",
		WINDOW_STYLE,
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
	return TRUE;
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
		UpdateWindow(HWND_OF(win));
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
void Gui_Pump(void)
{
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
}


REBOOL Gui_Get_Size(GUIWIN *win, REBINT *w, REBINT *h)
{
	RECT r;
	if (!win || !win->handle || !GetClientRect(HWND_OF(win), &r)) return FALSE;
	*w = r.right - r.left;
	*h = r.bottom - r.top;
	return TRUE;
}


REBOOL Gui_Get_Offset(GUIWIN *win, REBINT *x, REBINT *y)
{
	RECT r;
	if (!win || !win->handle || !GetWindowRect(HWND_OF(win), &r)) return FALSE;
	*x = r.left;
	*y = r.top;
	return TRUE;
}


REBOOL Gui_Set_Size(GUIWIN *win, REBINT w, REBINT h)
{
	RECT r;
	if (!win || !win->handle) return FALSE;

	r.left = 0; r.top = 0; r.right = w; r.bottom = h;
	AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(HWND_OF(win), GWL_STYLE),
	                   FALSE, (DWORD)GetWindowLongPtrW(HWND_OF(win), GWL_EXSTYLE));

	return SetWindowPos(HWND_OF(win), NULL, 0, 0,
	                    r.right - r.left, r.bottom - r.top,
	                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ? TRUE : FALSE;
}


REBOOL Gui_Set_Offset(GUIWIN *win, REBINT x, REBINT y)
{
	if (!win || !win->handle) return FALSE;
	return SetWindowPos(HWND_OF(win), NULL, x, y, 0, 0,
	                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) ? TRUE : FALSE;
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


//== widgets ==================================================================

// What a new control attaches to: the panel holding it, or the window.
// `wid->parent` is set before any creation call.
static HWND Parent_Hwnd(GUIWIDGET *wid, GUIWIN *owner)
{
	if (wid->parent && ((GUIWIDGET*)wid->parent)->handle)
		return (HWND)((GUIWIDGET*)wid->parent)->handle;
	return HWND_OF(owner);
}


REBOOL Gui_Create_Panel(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h,
                        const REBYTE *text, REBCNT len)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;
	if (!Register_Panel_Class()) return FALSE;

	// WS_CLIPCHILDREN keeps the panel from painting over what it holds.
	//
	// This is deliberately NOT a BS_GROUPBOX button, which is how Win32
	// usually draws a captioned frame: a group box is not a container here
	// but a sibling drawn behind other controls, and parenting children to
	// one is where the classic repaint and tab-order trouble comes from.
	// The panel keeps being a real container and draws the frame itself,
	// which is also what lets the frame be turned on and off later.
	hwnd = CreateWindowExW(
		0, Class_Name_Panel, L"",
		WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance, NULL
	);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);

	wid->handle = (void*)hwnd;

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

	switch (wid->kind) {
	case W_GUI_WIDGET_CHECK:
		// AUTO: the control ticks itself and we read the result back.
		style |= BS_AUTOCHECKBOX;
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
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Get_Default_Font(), TRUE);

	wid->handle = (void*)hwnd;
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
	DWORD  style   = WS_CHILD | WS_VISIBLE;
	DWORD  exstyle = 0;

	if (!wid || !owner || !owner->handle) return FALSE;

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
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Get_Default_Font(), TRUE);

	wid->handle = (void*)hwnd;
	return TRUE;
}


REBOOL Gui_Create_Image(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;
	if (!Register_Image_Class()) return FALSE;

	hwnd = CreateWindowExW(
		0,
		Class_Name_Image,
		L"",
		WS_CHILD | WS_VISIBLE,
		x, y, w, h,
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance,
		wid // arrives as lpCreateParams in WM_NCCREATE
	);
	if (!hwnd) return FALSE;

	wid->handle = (void*)hwnd;
	return TRUE;
}


void Gui_Widget_Redraw(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;
	InvalidateRect(HWND_OF_WID(wid), NULL, FALSE);
	// Painted now rather than whenever the queue next runs dry, so that
	// `redraw` means the pixels are on screen when it returns.
	UpdateWindow(HWND_OF_WID(wid));
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
	if (wid && wid->handle) DestroyWindow(HWND_OF_WID(wid));
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
	if (!font) font = Get_Default_Font();
	if (!font || !GetObjectW(font, sizeof(lf), &lf)) return FALSE;

	if (name && lf.lfFaceName[0]) {
		*name = RL_ENCODE_UTF8_STRING(lf.lfFaceName,
			(REBCNT)lstrlenW(lf.lfFaceName), TRUE, 0);
	}
	if (size) {
		// lfHeight is negative for a character height, positive for a cell
		// height; both are pixels, and points is what Rebol asked about.
		int pixels = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
		*size = (REBINT)MulDiv(pixels, 72, Screen_DPI());
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
	font = Font_For(wide, (int)size, style);
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
	// owner-drawn button can say otherwise.
	return (wid->kind == W_GUI_WIDGET_BUTTON) ? FALSE : TRUE;
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
		   + 2 * GetSystemMetrics(SM_CYEDGE);
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
	return TRUE;
}


REBOOL Gui_Widget_Set_Box(GUIWIDGET *wid, REBINT x, REBINT y, REBINT w, REBINT h)
{
	if (!wid || !wid->handle) return FALSE;
	// ... and the same room has to be added back when it is moved.
	if (wid->kind == W_GUI_WIDGET_DROP_DOWN) h += DROP_LIST_ROOM;
	return MoveWindow(HWND_OF_WID(wid), x, y, w, h, TRUE) ? TRUE : FALSE;
}


REBOOL Gui_Create_Range_Control(GUIWIDGET *wid, GUIWIN *owner,
                                REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND  hwnd;
	const WCHAR *class_name;
	DWORD style = WS_CHILD | WS_VISIBLE;

	if (!wid || !owner || !owner->handle) return FALSE;

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
	} else {
		SendMessageW(HWND_OF_WID(wid), PBM_SETPOS, (WPARAM)pos, 0);
	}
}


//-- drop-down ----------------------------------------------------------------

REBOOL Gui_Create_Drop_Down(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	HWND hwnd;

	if (!wid || !owner || !owner->handle) return FALSE;

	hwnd = CreateWindowExW(
		0, L"COMBOBOX", L"",
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL
		| CBS_DROPDOWNLIST | CBS_HASSTRINGS,
		x, y, w, h + DROP_LIST_ROOM, // see DROP_LIST_ROOM
		Parent_Hwnd(wid, owner),
		NULL,
		App_Instance, NULL
	);
	if (!hwnd) return FALSE;

	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)wid);
	SendMessageW(hwnd, WM_SETFONT, (WPARAM)Get_Default_Font(), TRUE);

	wid->handle = (void*)hwnd;
	return TRUE;
}


REBCNT Gui_Widget_Count_Items(GUIWIDGET *wid)
{
	LRESULT count;
	if (!wid || !wid->handle) return 0;
	count = SendMessageW(HWND_OF_WID(wid), CB_GETCOUNT, 0, 0);
	return (count == CB_ERR || count < 0) ? 0 : (REBCNT)count;
}


REBSER* Gui_Widget_Get_Item(GUIWIDGET *wid, REBCNT n)
{
	LRESULT len;
	WCHAR  *buf;
	REBSER *str;
	HWND    hwnd;

	if (!wid || !wid->handle) return NULL;
	hwnd = HWND_OF_WID(wid);

	len = SendMessageW(hwnd, CB_GETLBTEXTLEN, (WPARAM)n, 0);
	if (len == CB_ERR) return NULL;
	if (len == 0) return RL_MAKE_STRING(0, FALSE);

	buf = (WCHAR*)MAKE_MEM(((size_t)len + 1) * sizeof(WCHAR));
	if (!buf) return NULL;

	len = SendMessageW(hwnd, CB_GETLBTEXT, (WPARAM)n, (LPARAM)buf);
	if (len == CB_ERR) { FREE_MEM(buf); return NULL; }

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
	res = SendMessageW(HWND_OF_WID(wid), CB_ADDSTRING, 0,
	                   (LPARAM)(wide ? wide : L""));
	if (wide) FREE_MEM(wide);
	return (res == CB_ERR || res == CB_ERRSPACE) ? FALSE : TRUE;
}


void Gui_Widget_Clear_Items(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;
	SendMessageW(HWND_OF_WID(wid), CB_RESETCONTENT, 0, 0);
}


REBINT Gui_Widget_Get_Index(GUIWIDGET *wid)
{
	LRESULT n;
	if (!wid || !wid->handle) return -1;
	n = SendMessageW(HWND_OF_WID(wid), CB_GETCURSEL, 0, 0);
	return (n == CB_ERR) ? -1 : (REBINT)n;
}


void Gui_Widget_Set_Index(GUIWIDGET *wid, REBINT n)
{
	if (!wid || !wid->handle) return;
	// CB_SETCURSEL with -1 clears the selection, which is what an index
	// out of range means here.
	SendMessageW(HWND_OF_WID(wid), CB_SETCURSEL, (WPARAM)n, 0);
}


REBOOL Gui_Widget_Get_State(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	return (SendMessageW(HWND_OF_WID(wid), BM_GETCHECK, 0, 0) == BST_CHECKED)
		? TRUE : FALSE;
}


void Gui_Widget_Set_State(GUIWIDGET *wid, REBOOL on)
{
	if (!wid || !wid->handle) return;
	SendMessageW(HWND_OF_WID(wid), BM_SETCHECK,
	             on ? BST_CHECKED : BST_UNCHECKED, 0);
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
