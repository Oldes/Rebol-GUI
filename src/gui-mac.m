//
// Project: Rebol/GUI extension
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
// Cocoa backend.
//
// Implements exactly the same Gui_* API as gui-win.c, so nothing above it
// changes: commands, the event queue and the handle accessors are shared.
//
// Two things are converted at this boundary, because the API is defined in
// the Win32-ish terms the rest of the extension uses:
//
//   * coordinates. Cocoa puts the origin at the BOTTOM-left of the primary
//     screen with Y growing upwards; every offset crossing this file is
//     flipped to a top-left origin. Inside the window the flip is free -
//     the content view answers YES to isFlipped.
//
//   * ownership. The window is created with releasedWhenClosed:NO, so its
//     lifetime is ours and matches the handle's, not AppKit's.
//
// NOTE: sizes here are in POINTS, not pixels. On a Retina display a 640x480
// window has a 1280x960 backing store. That difference only starts to matter
// once something is drawn into the window, so the conversion is deliberately
// left out until there is a backing image to convert for.
//
// AppKit is imported before the Rebol headers on purpose: reb-c.h defines
// HAS_BOOL under __OBJC__ and guards TRUE/FALSE, so it adapts to MacTypes.h
// rather than fighting it.
//

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <math.h>
#include <string.h>
#include <float.h>

#include "gen-gui.h"
#include "gui.h"

// Modern AppKit constant names are used throughout (NSWindowStyleMask*,
// NSEventModifierFlag*, NSEventMaskAny, NSControlStateValue*), which need a
// 10.13 or newer SDK.
#define NSWINDOW_OF(win) ((NSWindow*)((win)->handle))


//== helpers ==================================================================

// Height of the screen holding the menu bar - the origin of the global
// coordinate space, and so the one to flip against.
static CGFloat Screen_Height(void)
{
	NSArray *screens = [NSScreen screens];
	if ([screens count] == 0) return 0.0;
	return [(NSScreen*)[screens objectAtIndex:0] frame].size.height;
}

static REBINT Modifier_Bits(NSEventModifierFlags mods)
{
	REBINT flags = 0;
	if (mods & NSEventModifierFlagShift)   flags |= GUI_FLAG_SHIFT;
	if (mods & NSEventModifierFlagControl) flags |= GUI_FLAG_CONTROL;
	if (mods & NSEventModifierFlagOption)  flags |= GUI_FLAG_ALT;
	return flags;
}

#define Modifiers(evt) Modifier_Bits([(evt) modifierFlags])

// UTF-8 (not necessarily terminated) -> an autoreleased NSString.
static NSString* To_NSString(const REBYTE *utf8, REBCNT len)
{
	if (!utf8 || len == 0) return nil;
	return [[[NSString alloc] initWithBytes:utf8
	                                 length:(NSUInteger)len
	                               encoding:NSUTF8StringEncoding] autorelease];
}

// ... and back: an NSString -> a fresh Rebol string series. The text is
// already UTF-8 here, so this is the decoding direction - the Windows
// backend, starting from UTF-16, encodes instead.
static REBSER* From_NSString(NSString *str)
{
	const char *utf8;
	if (!str) return NULL;
	utf8 = [str UTF8String];
	if (!utf8) return RL_MAKE_STRING(0, FALSE);
	return RL_DECODE_UTF_STRING((REBYTE*)utf8, (REBCNT)strlen(utf8), 8, FALSE, FALSE);
}


// Defined with the widget code below, but needed by Gui_Close_Window above it.
static void Detach_Control(GUIWIDGET *wid);


//== content view =============================================================
//
// One object in two roles: the window's content view (where mouse events
// arrive) and its delegate (where close and resize arrive). Keeping them
// together means a single back pointer to the GUIWIN, and one object to
// tear down.

@interface RebolGuiView : NSView <NSWindowDelegate>
{
	GUIWIN *context;
	NSTrackingArea *tracking;
}
// Named apart from the controls' setContext: on purpose - this one takes a
// GUIWIN, theirs take a GUIWIDGET, and a selector sent to `id` must have one
// unambiguous signature across every class which declares it.
- (void)setWindowContext:(GUIWIN*)ctx;
@end


//== controls =================================================================
//
// The button is its own target: AppKit offers no user-data slot on a control,
// and a subclass with one ivar is cheaper than a lookup table keyed by tag.

@interface RebolGuiButton : NSButton
{
	GUIWIDGET *context;
}
- (void)setContext:(GUIWIDGET*)ctx;
- (void)clicked:(id)sender;
@end


// Static labels and one-line entries are the same class with different
// switches; each is its own delegate, so that editing reports back without
// a separate object to keep alive.
@interface RebolGuiTextField : NSTextField <NSTextFieldDelegate>
{
	GUIWIDGET *context;
}
- (void)setContext:(GUIWIDGET*)ctx;
@end


// The multi-line entry. It lives inside an NSScrollView, and that scroll
// view - not this - is what the widget handle points at.
@interface RebolGuiTextView : NSTextView <NSTextViewDelegate>
{
	GUIWIDGET *context;
}
- (void)setContext:(GUIWIDGET*)ctx;
@end


// Paints itself straight from the image! series and reports its own mouse
// events, which is what makes it usable as a canvas rather than a picture.
@interface RebolGuiImageView : NSView
{
	GUIWIDGET *context;
	NSTrackingArea *tracking;
}
- (void)setContext:(GUIWIDGET*)ctx;
@end


@implementation RebolGuiButton

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (void)clicked:(id)sender
{
	NSRect frame;
	if (!context || !context->hob) return;

	// No cursor position comes with an action message, so the position slot
	// carries the button's own offset - the same as WM_COMMAND on Windows.
	//
	// Not queued directly: a check or a radio has state to settle first,
	// and radio grouping is decided above this file.
	frame = [self frame];
	Gui_Widget_Activated(context, (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                     Modifier_Bits([NSEvent modifierFlags]));
}

@end


@implementation RebolGuiTextField

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

// A notification has no cursor position, so - as with a button - the
// position slot carries the control's own offset.
- (void)queue:(REBCNT)type
{
	NSRect frame;
	if (!context || !context->hob) return;
	frame = [self frame];
	Gui_Queue_Event(context->hob, type,
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                Modifier_Bits([NSEvent modifierFlags]));
}

// These fire for USER edits only - setStringValue: does not call them, so
// unlike Win32's EN_CHANGE there is nothing to suppress when Rebol writes
// to the control.
- (void)controlTextDidChange:(NSNotification*)note       { [self queue:W_GUI_EVENT_CHANGE]; }
- (void)controlTextDidBeginEditing:(NSNotification*)note { [self queue:W_GUI_EVENT_FOCUS]; }
- (void)controlTextDidEndEditing:(NSNotification*)note   { [self queue:W_GUI_EVENT_UNFOCUS]; }

@end


@implementation RebolGuiTextView

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (void)queue:(REBCNT)type
{
	NSRect frame;
	if (!context || !context->hob) return;
	// The scroll view is what sits in the window, so its box is the one
	// worth reporting - not this view's, which scrolls.
	frame = [[self enclosingScrollView] frame];
	Gui_Queue_Event(context->hob, type,
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                Modifier_Bits([NSEvent modifierFlags]));
}

- (void)textDidChange:(NSNotification*)note       { [self queue:W_GUI_EVENT_CHANGE]; }
- (void)textDidBeginEditing:(NSNotification*)note { [self queue:W_GUI_EVENT_FOCUS]; }
- (void)textDidEndEditing:(NSNotification*)note   { [self queue:W_GUI_EVENT_UNFOCUS]; }

@end


@implementation RebolGuiImageView

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)evt { return YES; }

- (void)dealloc
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		[tracking release];
		tracking = nil;
	}
	[super dealloc];
}

// Its own tracking area, for the same reason the window's view has one:
// without it mouseMoved: never reaches this view.
- (void)updateTrackingAreas
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		[tracking release];
	}
	tracking = [[NSTrackingArea alloc]
		initWithRect:[self bounds]
		     options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
		       owner:self
		    userInfo:nil];
	[self addTrackingArea:tracking];
	[super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirty
{
	REBYTE *bits = NULL;
	REBINT  w = 0, h = 0;
	NSRect  bounds = [self bounds];
	CGContextRef gc;
	CGColorSpaceRef space;
	CGDataProviderRef provider;
	CGImageRef img;

	if (!context || !Gui_Widget_Pixels(context, &bits, &w, &h)) {
		[[NSColor windowBackgroundColor] set];
		NSRectFill(dirty);
		return;
	}

	gc = [[NSGraphicsContext currentContext] CGContext];
	space = CGColorSpaceCreateDeviceRGB();

	// No copy: the provider reads the image! series in place, and nothing
	// outlives this call.
	provider = CGDataProviderCreateWithData(NULL, bits, (size_t)w * (size_t)h * 4, NULL);

	// image! is BGRA in memory. On a little-endian machine that is exactly
	// "32 bits little-endian, alpha in the high byte, skipped".
	img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, space,
	                    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little,
	                    provider, NULL, false, kCGRenderingIntentDefault);

	if (img) {
		CGContextSaveGState(gc);
		// The view is flipped and the CGImage is not, so the axis is put
		// back the other way round just for the draw.
		CGContextTranslateCTM(gc, 0, bounds.size.height);
		CGContextScaleCTM(gc, 1, -1);
		CGContextSetInterpolationQuality(gc, kCGInterpolationNone);
		CGContextDrawImage(gc, CGRectMake(0, 0, bounds.size.width, bounds.size.height), img);
		CGContextRestoreGState(gc);
		CGImageRelease(img);
	}

	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
}

- (void)queue:(REBCNT)type from:(NSEvent*)evt extra:(REBINT)extra
{
	NSPoint pt;
	if (!context || !context->hob) return;
	pt = [self convertPoint:[evt locationInWindow] fromView:nil];
	Gui_Queue_Event(context->hob, type,
	                (REBINT)floor(pt.x), (REBINT)floor(pt.y),
	                Modifiers(evt) | extra);
}

// A subview covers its part of the window, so the window's own view stops
// hearing about the mouse there - these report it instead, with the widget
// as the source and coordinates of its own.
- (void)mouseMoved:(NSEvent*)evt        { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }
- (void)mouseDragged:(NSEvent*)evt      { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }
- (void)rightMouseDragged:(NSEvent*)evt { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }

- (void)mouseDown:(NSEvent*)evt
{
	[self queue:W_GUI_EVENT_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)mouseUp:(NSEvent*)evt       { [self queue:W_GUI_EVENT_UP from:evt extra:0]; }
- (void)rightMouseDown:(NSEvent*)evt{ [self queue:W_GUI_EVENT_ALT_DOWN from:evt extra:0]; }
- (void)rightMouseUp:(NSEvent*)evt  { [self queue:W_GUI_EVENT_ALT_UP from:evt extra:0]; }
- (void)otherMouseDown:(NSEvent*)evt{ [self queue:W_GUI_EVENT_AUX_DOWN from:evt extra:0]; }
- (void)otherMouseUp:(NSEvent*)evt  { [self queue:W_GUI_EVENT_AUX_UP from:evt extra:0]; }

@end


@implementation RebolGuiView

- (void)setWindowContext:(GUIWIN*)ctx { context = ctx; }

// Top-left origin, Y growing down - the same convention as the rest of the
// extension, so no per-event flipping is needed inside the window.
- (BOOL)isFlipped          { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

// Clicking an inactive window should deliver the click, not just raise it.
- (BOOL)acceptsFirstMouse:(NSEvent*)evt { return YES; }

- (void)dealloc
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		[tracking release];
		tracking = nil;
	}
	[super dealloc];
}

// A tracking area is what makes mouseMoved: fire; NSTrackingActiveAlways is
// chosen so that movement is reported even when the window is not key,
// matching WM_MOUSEMOVE on Windows.
- (void)updateTrackingAreas
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		[tracking release];
	}
	tracking = [[NSTrackingArea alloc]
		initWithRect:[self bounds]
		     options:(NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect)
		       owner:self
		    userInfo:nil];
	[self addTrackingArea:tracking];
	[super updateTrackingAreas];
}

- (void)queue:(REBCNT)type from:(NSEvent*)evt extra:(REBINT)extra
{
	NSPoint pt;
	if (!context || !context->hob) return;
	pt = [self convertPoint:[evt locationInWindow] fromView:nil];
	Gui_Queue_Event(context->hob, type,
	                (REBINT)floor(pt.x), (REBINT)floor(pt.y),
	                Modifiers(evt) | extra);
}

//-- mouse motion -------------------------------------------------------------
// Cocoa reports movement with a button held as a drag rather than a move.
// Windows makes no such distinction (it captures the mouse instead), so all
// four are reported here as plain `move` events.

- (void)mouseMoved:(NSEvent*)evt        { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }
- (void)mouseDragged:(NSEvent*)evt      { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }
- (void)rightMouseDragged:(NSEvent*)evt { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }
- (void)otherMouseDragged:(NSEvent*)evt { [self queue:W_GUI_EVENT_MOVE from:evt extra:0]; }

//-- buttons ------------------------------------------------------------------

- (void)mouseDown:(NSEvent*)evt
{
	[self queue:W_GUI_EVENT_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)mouseUp:(NSEvent*)evt           { [self queue:W_GUI_EVENT_UP from:evt extra:0]; }

- (void)rightMouseDown:(NSEvent*)evt
{
	[self queue:W_GUI_EVENT_ALT_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)rightMouseUp:(NSEvent*)evt      { [self queue:W_GUI_EVENT_ALT_UP from:evt extra:0]; }

- (void)otherMouseDown:(NSEvent*)evt
{
	[self queue:W_GUI_EVENT_AUX_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)otherMouseUp:(NSEvent*)evt      { [self queue:W_GUI_EVENT_AUX_UP from:evt extra:0]; }

//-- wheel --------------------------------------------------------------------

- (void)scrollWheel:(NSEvent*)evt
{
	NSPoint pt;
	CGFloat dy;
	REBINT lines;

	if (!context || !context->hob) return;

	dy = [evt scrollingDeltaY];
	// A trackpad or a Magic Mouse reports pixel-precise deltas; a wheel
	// reports whole lines already.
	if ([evt hasPreciseScrollingDeltas]) dy /= 10.0;

	// Sub-line movement still has to say which way it went, hence the
	// rounding away from zero.
	lines = (REBINT)(dy > 0 ? ceil(dy) : floor(dy));
	if (lines == 0) return;

	// NOTE: this follows the user's "natural scrolling" setting, exactly as
	// every other Mac application does - the sign is not normalised against
	// [evt isDirectionInvertedFromDevice].
	pt = [self convertPoint:[evt locationInWindow] fromView:nil];
	Gui_Queue_Event(context->hob, W_GUI_EVENT_WHEEL,
	                (REBINT)floor(pt.x), (REBINT)floor(pt.y), lines);
}

//-- window delegate ----------------------------------------------------------

// Reported only; the window stays open until Rebol calls `close-window`.
- (BOOL)windowShouldClose:(id)sender
{
	if (context && context->hob)
		Gui_Queue_Event(context->hob, W_GUI_EVENT_CLOSE, 0, 0, 0);
	return NO;
}

- (void)windowDidResize:(NSNotification*)note
{
	NSSize size;
	if (!context || !context->hob) return;
	size = [self bounds].size;
	Gui_Queue_Event(context->hob, W_GUI_EVENT_RESIZE,
	                (REBINT)size.width, (REBINT)size.height, 0);
}

@end


//== platform API =============================================================

/***********************************************************************
**  One-time process setup.
**
**  A Rebol built as a plain command line tool has no NSApplication and
**  no activation policy, so a window it opens would never come to the
**  front and would never receive events. Both are set up here.
**
**  NOTE: AppKit requires this - and every call below - on the main
**  thread, which is where the interpreter runs. No menu bar is
**  installed: a Quit item would terminate the process behind Rebol's
**  back, which is exactly what this extension is trying to avoid.
***********************************************************************/
void Gui_Init_Platform(void)
{
	@autoreleasepool {
		if (![NSThread isMainThread]) return;

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		// Without this a non-bundled process never finishes launching, and
		// its windows stay unresponsive.
		[NSApp finishLaunching];
	}
}


void Gui_Quit_Platform(void)
{
	// Windows still open belong to a process which is going away; AppKit
	// tears them down with it. Nothing to unregister, unlike a window class.
}


REBOOL Gui_Open_Window(GUIWIN *win, REBINT x, REBINT y, REBINT w, REBINT h,
                       const REBYTE *title, REBCNT title_len)
{
	@autoreleasepool {
		NSWindow *window;
		RebolGuiView *view;
		NSString *name;
		NSRect rect = NSMakeRect(0, 0, (CGFloat)w, (CGFloat)h);
		NSUInteger style = NSWindowStyleMaskTitled
		                 | NSWindowStyleMaskClosable
		                 | NSWindowStyleMaskMiniaturizable
		                 | NSWindowStyleMaskResizable;

		if (![NSThread isMainThread]) return FALSE;

		// The rect is the CONTENT rect, so the requested size is the usable
		// area - the same promise the Windows backend makes.
		window = [[NSWindow alloc] initWithContentRect:rect
		                                     styleMask:style
		                                       backing:NSBackingStoreBuffered
		                                         defer:NO];
		if (!window) return FALSE;

		// Ours to release, not AppKit's to drop on close.
		[window setReleasedWhenClosed:NO];
		[window setAcceptsMouseMovedEvents:YES];

		name = To_NSString(title, title_len);
		[window setTitle:(name ? name : @"Rebol")];

		view = [[RebolGuiView alloc] initWithFrame:rect];
		[view setWindowContext:win];
		[window setContentView:view];
		[window setDelegate:view];
		[window makeFirstResponder:view];
		[view release]; // the window retains both roles

		if (x == GUI_DEFAULT_POS || y == GUI_DEFAULT_POS) {
			[window center];
		} else {
			// Top-left origin, Y down -> Cocoa's bottom-left origin, Y up.
			[window setFrameTopLeftPoint:
				NSMakePoint((CGFloat)x, Screen_Height() - (CGFloat)y)];
		}

		win->handle = (void*)window;
		win->flags  = 0;
		return TRUE;
	}
}


void Gui_Close_Window(GUIWIN *win)
{
	@autoreleasepool {
		NSWindow *window;

		if (!win || !win->handle) return;
		window = NSWINDOW_OF(win);

		// Cleared first: the delegate callbacks must not find a window which
		// is half torn down.
		win->handle = NULL;
		win->flags &= ~GUIW_VISIBLE;

		// AppKit would drop the subviews along with the window, but this
		// file holds a reference to each control, so they are handed back
		// before the window goes. Gui_Window_Closed() then deals with the
		// Rebol side of the same widgets.
		{
			GUIWIDGET *wid = (GUIWIDGET*)win->widgets;
			while (wid) {
				if (wid->handle) {
					NSView *control = (NSView*)wid->handle;
					Detach_Control(wid);
					wid->handle = NULL;
					[control removeFromSuperview];
					[control release];
				}
				wid = (GUIWIDGET*)wid->next;
			}
		}

		[(RebolGuiView*)[window contentView] setWindowContext:NULL];
		[window setDelegate:nil];
		[window orderOut:nil];
		[window close];
		[window release];

		// Same contract as WM_NCDESTROY on Windows: the window is really
		// gone, so drop its queued events and unlock the handle.
		if (win->hob) Gui_Window_Closed(win->hob);
	}
}


void Gui_Show_Window(GUIWIN *win, REBOOL show)
{
	@autoreleasepool {
		if (!win || !win->handle) return;
		if (show) {
			[NSWINDOW_OF(win) makeKeyAndOrderFront:nil];
			[NSApp activateIgnoringOtherApps:YES];
			win->flags |= GUIW_VISIBLE;
		} else {
			[NSWINDOW_OF(win) orderOut:nil];
			win->flags &= ~GUIW_VISIBLE;
		}
	}
}


/***********************************************************************
**  Drains the application's event queue.
**
**  NOTE: like the Windows pump, this takes EVERY event of the process,
**  not only those of our windows - which is what lets a plain
**  `poll-events` loop work without a host side event device.
***********************************************************************/
void Gui_Pump(void)
{
	@autoreleasepool {
		NSEvent *evt;

		if (![NSThread isMainThread]) return;

		while ((evt = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                 untilDate:[NSDate distantPast]
		                                    inMode:NSDefaultRunLoopMode
		                                   dequeue:YES]))
		{
			[NSApp sendEvent:evt];
		}
		[NSApp updateWindows];
	}
}


REBOOL Gui_Get_Size(GUIWIN *win, REBINT *w, REBINT *h)
{
	@autoreleasepool {
		NSSize size;
		if (!win || !win->handle) return FALSE;
		size = [[NSWINDOW_OF(win) contentView] bounds].size;
		*w = (REBINT)size.width;
		*h = (REBINT)size.height;
		return TRUE;
	}
}


REBOOL Gui_Get_Offset(GUIWIN *win, REBINT *x, REBINT *y)
{
	@autoreleasepool {
		NSRect frame;
		if (!win || !win->handle) return FALSE;
		frame = [NSWINDOW_OF(win) frame];
		*x = (REBINT)frame.origin.x;
		// Cocoa's top edge, measured down from the top of the main screen.
		*y = (REBINT)(Screen_Height() - (frame.origin.y + frame.size.height));
		return TRUE;
	}
}


REBOOL Gui_Set_Size(GUIWIN *win, REBINT w, REBINT h)
{
	@autoreleasepool {
		NSWindow *window;
		NSRect frame;
		CGFloat top;

		if (!win || !win->handle) return FALSE;
		window = NSWINDOW_OF(win);

		// Resizing a Cocoa window keeps its BOTTOM-left corner in place,
		// which looks like the window jumping upwards to anyone thinking in
		// top-left coordinates. The top edge is pinned instead.
		frame = [window frame];
		top = frame.origin.y + frame.size.height;

		frame = [window frameRectForContentRect:
			NSMakeRect(frame.origin.x, 0, (CGFloat)w, (CGFloat)h)];
		frame.origin.y = top - frame.size.height;

		[window setFrame:frame display:YES];
		return TRUE;
	}
}


REBOOL Gui_Set_Offset(GUIWIN *win, REBINT x, REBINT y)
{
	@autoreleasepool {
		if (!win || !win->handle) return FALSE;
		[NSWINDOW_OF(win) setFrameTopLeftPoint:
			NSMakePoint((CGFloat)x, Screen_Height() - (CGFloat)y)];
		return TRUE;
	}
}


REBSER* Gui_Get_Title(GUIWIN *win)
{
	@autoreleasepool {
		if (!win || !win->handle) return NULL;
		return From_NSString([NSWINDOW_OF(win) title]);
	}
}


REBOOL Gui_Set_Title(GUIWIN *win, const REBYTE *utf8, REBCNT len)
{
	@autoreleasepool {
		NSString *name;
		if (!win || !win->handle) return FALSE;
		name = To_NSString(utf8, len);
		[NSWINDOW_OF(win) setTitle:(name ? name : @"")];
		return TRUE;
	}
}


//== widgets ==================================================================

// Geometry is the same for every kind of control, so it goes through NSView;
// only the label and the enabled state need the concrete class.
#define NSVIEW_OF(wid)   ((NSView*)((wid)->handle))
#define NSBUTTON_OF(wid) ((RebolGuiButton*)((wid)->handle))

// An area's handle is its scroll view; the text itself lives one level in.
static NSTextView* Text_View_Of(GUIWIDGET *wid)
{
	if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_AREA) return nil;
	return (NSTextView*)[(NSScrollView*)wid->handle documentView];
}

// Breaks the link from a native control back to its Rebol handle. The
// scroll view of an area does not answer setContext: - the view inside it
// is the one holding the pointer.
static void Detach_Control(GUIWIDGET *wid)
{
	NSView *view;

	if (!wid || !wid->handle) return;
	view = NSVIEW_OF(wid);

	if (wid->kind == W_GUI_WIDGET_AREA) {
		NSTextView *tv = (NSTextView*)[(NSScrollView*)view documentView];
		if (tv) {
			[tv setDelegate:nil];
			[(id)tv setContext:NULL];
		}
	} else if ([view respondsToSelector:@selector(setContext:)]) {
		[(id)view setContext:NULL];
	}
}

REBOOL Gui_Create_Button_Control(GUIWIDGET *wid, GUIWIN *owner,
                                 REBINT x, REBINT y, REBINT w, REBINT h,
                                 const REBYTE *text, REBCNT len)
{
	@autoreleasepool {
		RebolGuiButton *button;
		NSString *label;
		NSView *content;

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = [NSWINDOW_OF(owner) contentView];
		if (!content) return FALSE;

		// The content view is flipped, so the frame origin is the top-left
		// corner - no conversion needed for child geometry.
		button = [[RebolGuiButton alloc] initWithFrame:
			NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)];
		if (!button) return FALSE;

		switch (wid->kind) {
		case W_GUI_WIDGET_CHECK:
			[button setButtonType:NSButtonTypeSwitch];
			break;
		case W_GUI_WIDGET_RADIO:
			// AppKit groups radios sharing a superview and an action, which
			// here is every radio in the window. It still clears them when
			// one is CLICKED; Gui_Widget_Set_State() below is what puts the
			// wrongly cleared ones back without setting the same trap off
			// again.
			[button setButtonType:NSButtonTypeRadio];
			break;
		default:
			[button setBezelStyle:NSBezelStyleRounded];
			[button setButtonType:NSButtonTypeMomentaryPushIn];
			break;
		}

		label = To_NSString(text, len);
		[button setTitle:(label ? label : @"")];

		[button setContext:wid];
		[button setTarget:button];
		[button setAction:@selector(clicked:)];

		// The superview retains it as well; the reference kept here is the
		// one Gui_Destroy_Widget gives back.
		[content addSubview:button];

		wid->handle = (void*)button;
		return TRUE;
	}
}


REBOOL Gui_Create_Text_Control(GUIWIDGET *wid, GUIWIN *owner,
                               REBINT x, REBINT y, REBINT w, REBINT h,
                               const REBYTE *text, REBCNT len)
{
	@autoreleasepool {
		NSView   *content;
		NSString *value;
		NSRect    rect = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = [NSWINDOW_OF(owner) contentView];
		if (!content) return FALSE;

		value = To_NSString(text, len);
		if (!value) value = @"";

		if (wid->kind == W_GUI_WIDGET_AREA) {
			NSScrollView *scroll;
			RebolGuiTextView *tv;
			NSSize inner;

			scroll = [[NSScrollView alloc] initWithFrame:rect];
			if (!scroll) return FALSE;

			[scroll setBorderType:NSBezelBorder];
			[scroll setHasVerticalScroller:YES];
			[scroll setHasHorizontalScroller:NO];
			[scroll setAutohidesScrollers:YES];

			// The standard incantation for a text view which grows
			// downwards and wraps to the width of its scroller.
			inner = [scroll contentSize];
			tv = [[RebolGuiTextView alloc] initWithFrame:
				NSMakeRect(0, 0, inner.width, inner.height)];
			if (!tv) { [scroll release]; return FALSE; }

			[tv setMinSize:NSMakeSize(0.0, 0.0)];
			[tv setMaxSize:NSMakeSize(FLT_MAX, FLT_MAX)];
			[tv setVerticallyResizable:YES];
			[tv setHorizontallyResizable:NO];
			[tv setAutoresizingMask:NSViewWidthSizable];
			[[tv textContainer] setContainerSize:NSMakeSize(inner.width, FLT_MAX)];
			[[tv textContainer] setWidthTracksTextView:YES];

			[tv setString:value];
			[tv setContext:wid];
			[tv setDelegate:tv]; // the delegate reference is not retained

			[scroll setDocumentView:tv];
			[tv release]; // the scroll view owns it now

			[content addSubview:scroll];
			wid->handle = (void*)scroll;
			return TRUE;
		}

		{
			RebolGuiTextField *field = [[RebolGuiTextField alloc] initWithFrame:rect];
			if (!field) return FALSE;

			[field setStringValue:value];

			if (wid->kind == W_GUI_WIDGET_TEXT) {
				// A label is a text field with everything switched off.
				[field setEditable:NO];
				[field setSelectable:NO];
				[field setBezeled:NO];
				[field setDrawsBackground:NO];
			} else {
				[field setEditable:YES];
				[field setBezeled:YES];
				[field setBezelStyle:NSTextFieldSquareBezel];
			}

			[field setContext:wid];
			[field setDelegate:field];

			[content addSubview:field];
			wid->handle = (void*)field;
			return TRUE;
		}
	}
}


REBOOL Gui_Create_Image(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h)
{
	@autoreleasepool {
		RebolGuiImageView *view;
		NSView *content;

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = [NSWINDOW_OF(owner) contentView];
		if (!content) return FALSE;

		view = [[RebolGuiImageView alloc] initWithFrame:
			NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)];
		if (!view) return FALSE;

		[view setContext:wid];
		[content addSubview:view];

		wid->handle = (void*)view;
		return TRUE;
	}
}


void Gui_Widget_Redraw(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		// Drawn now rather than at the next turn of the run loop, so that
		// `redraw` means the pixels are on screen when it returns.
		[NSVIEW_OF(wid) displayIfNeeded];
	}
}


void Gui_Window_Redraw(GUIWIN *win)
{
	@autoreleasepool {
		NSView *content;
		if (!win || !win->handle) return;
		content = [NSWINDOW_OF(win) contentView];
		[content setNeedsDisplay:YES];
		[content displayIfNeeded];
	}
}


void Gui_Destroy_Widget(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSView *view;

		// A NULL handle means the window already took the control with it.
		if (!wid || !wid->handle) return;

		view = NSVIEW_OF(wid);
		Detach_Control(wid);
		wid->handle = NULL;
		[view removeFromSuperview];
		[view release];
	}
}


REBSER* Gui_Widget_Get_Text(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return NULL;

		switch (wid->kind) {
		case W_GUI_WIDGET_AREA:
			return From_NSString([Text_View_Of(wid) string]);
		case W_GUI_WIDGET_TEXT:
		case W_GUI_WIDGET_FIELD:
			return From_NSString([(NSTextField*)wid->handle stringValue]);
		default:
			return From_NSString([NSBUTTON_OF(wid) title]);
		}
	}
}


REBOOL Gui_Widget_Set_Text(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	@autoreleasepool {
		NSString *value;

		if (!wid || !wid->handle) return FALSE;
		value = To_NSString(utf8, len);
		if (!value) value = @"";

		switch (wid->kind) {
		case W_GUI_WIDGET_AREA:
			[Text_View_Of(wid) setString:value];
			break;
		case W_GUI_WIDGET_TEXT:
		case W_GUI_WIDGET_FIELD:
			[(NSTextField*)wid->handle setStringValue:value];
			break;
		default:
			[NSBUTTON_OF(wid) setTitle:value];
			break;
		}
		return TRUE;
	}
}


REBOOL Gui_Widget_Get_Box(GUIWIDGET *wid, REBINT *x, REBINT *y, REBINT *w, REBINT *h)
{
	@autoreleasepool {
		NSRect frame;
		if (!wid || !wid->handle) return FALSE;
		frame = [NSVIEW_OF(wid) frame];
		*x = (REBINT)frame.origin.x;
		*y = (REBINT)frame.origin.y;
		*w = (REBINT)frame.size.width;
		*h = (REBINT)frame.size.height;
		return TRUE;
	}
}


REBOOL Gui_Widget_Set_Box(GUIWIDGET *wid, REBINT x, REBINT y, REBINT w, REBINT h)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		[NSVIEW_OF(wid) setFrame:
			NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)];
		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		return TRUE;
	}
}


REBOOL Gui_Widget_Get_State(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		return ([NSBUTTON_OF(wid) state] == NSControlStateValueOn) ? TRUE : FALSE;
	}
}


/***********************************************************************
**  Writes the state WITHOUT letting AppKit treat it as a group member.
**
**  Radio buttons sharing a superview and an action are a group to
**  AppKit, and switching one on turns the others off - every other
**  radio in the window, not just the ones this extension calls a group.
**  Worse, that happens on a programmatic setState: too, so re-asserting
**  the others afterwards only moves the problem: whichever radio is
**  written last is the only one left on.
**
**  Detaching the action for the length of the write takes the button
**  out of any group AppKit can see, so the write means exactly what it
**  says. The action is put straight back; nothing else can run in
**  between, since this is the main thread.
***********************************************************************/
void Gui_Widget_Set_State(GUIWIDGET *wid, REBOOL on)
{
	@autoreleasepool {
		RebolGuiButton *button;
		SEL action;

		if (!wid || !wid->handle) return;
		button = NSBUTTON_OF(wid);

		action = [button action];
		[button setAction:NULL];
		[button setState:(on ? NSControlStateValueOn : NSControlStateValueOff)];
		[button setAction:action];
	}
}


REBOOL Gui_Widget_Get_Enabled(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		// A scroll view has no enabled state; for an area the question is
		// whether the text inside it can be edited.
		if (wid->kind == W_GUI_WIDGET_AREA)
			return [Text_View_Of(wid) isEditable] ? TRUE : FALSE;
		return [(NSControl*)wid->handle isEnabled] ? TRUE : FALSE;
	}
}


REBOOL Gui_Widget_Set_Enabled(GUIWIDGET *wid, REBOOL enabled)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		if (wid->kind == W_GUI_WIDGET_AREA) {
			[Text_View_Of(wid) setEditable:(enabled ? YES : NO)];
			[Text_View_Of(wid) setSelectable:YES];
		} else {
			[(NSControl*)wid->handle setEnabled:(enabled ? YES : NO)];
		}
		return TRUE;
	}
}
