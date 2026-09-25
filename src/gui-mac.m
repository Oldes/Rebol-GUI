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
#include <stdio.h>  // snprintf
#include <float.h>

#include "gen-gui.h"
#include "gui.h"

// Modern AppKit constant names are used throughout (NSWindowStyleMask*,
// NSEventModifierFlag*, NSEventMaskAny, NSControlStateValue*), which need a
// 10.13 or newer SDK.


/***********************************************************************
**  Class names.
**
**  Objective-C classes live in ONE namespace for the whole process, and
**  it is not the linker's - two binaries which define a class of the
**  same name do not each get their own. The runtime keeps one and warns:
**
**    objc: Class RebolGuiButton is implemented in both
**          .../rebol3-bulk-macos-arm64 and .../gui-abi1-arm64.rebx.
**          This may cause spurious casting failures and mysterious
**          crashes. One of the duplicates must be removed or renamed.
**
**  That warning is not cosmetic. Every message sent from this file can
**  land in the OTHER implementation, which may have different ivars and
**  different methods - so a control half works, or does not draw, and
**  nothing in this file looks wrong.
**
**  It bites whenever these sources are loaded twice in one process: a
**  standalone .rebx while an older copy is embedded in the host, most
**  obviously. So the names carry a prefix the build can set, and the
**  standalone build sets a different one from the embedded default (see
**  Rebol-GUI.nest). Two builds that disagree about the prefix can coexist;
**  two that agree cannot.
***********************************************************************/
#ifndef GUI_CLASS_PREFIX
#define GUI_CLASS_PREFIX RebolGuiEmbedded
#endif
#define GUI_JOIN2(a, b) a##b
#define GUI_JOIN(a, b)  GUI_JOIN2(a, b)
#define GUI_CLASS(name) GUI_JOIN(GUI_CLASS_PREFIX, name)

// Written with the plain names below; the preprocessor makes them unique.
#define RebolGuiView      GUI_CLASS(View)
#define RebolGuiButton    GUI_CLASS(Button)
#define RebolGuiTextField GUI_CLASS(TextField)
#define RebolGuiSecureField GUI_CLASS(SecureField)
#define RebolGuiTextView  GUI_CLASS(TextView)
#define RebolGuiImageView GUI_CLASS(ImageView)
#define RebolGuiSlider    GUI_CLASS(Slider)
#define RebolGuiPopUp     GUI_CLASS(PopUp)
#define RebolGuiList      GUI_CLASS(List)
#define RebolGuiDatePicker GUI_CLASS(DatePicker)
#define RebolGuiPanel     GUI_CLASS(Panel)
#define RebolGuiWindow    GUI_CLASS(Window)
#define NSWINDOW_OF(win) ((NSWindow*)((win)->handle))
#define NSPANEL_OF(wid)  ((RebolGuiPanel*)((wid)->handle))

// How far in from the left edge a framed panel's caption starts. The same
// number as in gui-win.c, so the two look alike even though each measures
// the text with its own font.
#define PANEL_CAPTION_X 9

// Raised whenever anything is marked for drawing, cleared by the pump.
//
// Forcing a display from outside the run loop does not reliably paint:
// AppKit expects to do it itself, between events, and a display asked for
// at the wrong moment can clear a view's needs-display flag without
// putting anything on screen - after which nothing marks it again and the
// control stays blank for good. Widgets created in one burst, with no
// event pumped in between, hit this: the earlier ones never appear.
//
// So nothing here displays anything any more. Views are marked, this flag
// is raised, and Gui_Pump() - which runs between events, where AppKit
// expects drawing to happen - does one full window display. The cost is
// one redraw per poll in which something actually changed.
static REBOOL Display_Pending = FALSE;


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

// Likewise: a button's colour lives in its attributed title, so anything
// which replaces that title - new text, a new font - has to rebuild it.
static void Apply_Button_Color(GUIWIDGET *wid);


/***********************************************************************
**  The window.
**
**  A plain NSWindow would do for a titled one - this exists for the
**  BORDERLESS case. AppKit refuses to make a borderless window key or
**  main, and a window which cannot become key has no field editor: a
**  `field` or an `area` in it could be clicked and never typed into.
**  Two overrides fix that, and they say only what a titled window
**  already answers, so every window here is one of these.
***********************************************************************/

@interface RebolGuiWindow : NSWindow
@end

@implementation RebolGuiWindow
- (BOOL)canBecomeKeyWindow  { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end


//== content view =============================================================
//
// One object in two roles: the window's content view (where mouse events
// arrive) and its delegate (where close and resize arrive). Keeping them
// together means a single back pointer to the GUIWIN, and one object to
// tear down.

@interface RebolGuiView : NSView <NSWindowDelegate, NSDraggingDestination>
{
	GUIWIN *context;
	NSTrackingArea *tracking;
}
// Named apart from the controls' setContext: on purpose - this one takes a
// GUIWIN, theirs take a GUIWIDGET, and a selector sent to `id` must have one
// unambiguous signature across every class which declares it.
- (void)setWindowContext:(GUIWIN*)ctx;
// Menu items target the window's content view: it is the one object which
// already knows the GUIWIN, and it lives exactly as long as the window.
- (void)menuPicked:(id)sender;
- (void)quitPicked:(id)sender;
@end


//== controls =================================================================
//
// The button is its own target: AppKit offers no user-data slot on a control,
// and a subclass with one ivar is cheaper than a lookup table keyed by tag.

@interface RebolGuiButton : NSButton
{
	GUIWIDGET *context;
	BOOL       pressing;  // between `down` and its `up`
	BOOL       inside;    // the pointer is over it, so releasing clicks
	CGFloat    plain;     // height of the standard push bezel; 0 = not yet known
}
- (void)setContext:(GUIWIDGET*)ctx;
- (void)clicked:(id)sender;
- (NSSize)plainFittingSize;
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

// A `/secure` field. It has to BE an NSSecureTextField: the secure cell
// refuses to run - it throws on the first click - unless its field
// editor's delegate is one. The body is the same as the plain field's,
// shared through TEXT_FIELD_BODY below.
@interface RebolGuiSecureField : NSSecureTextField <NSTextFieldDelegate>
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


// A container, and - when asked for an edge - a frame around itself. It is
// flipped for the same reason the window's content view is: so that what it
// holds is positioned from the top-left, like every other offset in this
// extension.
@interface RebolGuiPanel : NSView
{
	GUIWIDGET *context;  // read at paint time for GUI_PANEL_BORDER and colour
	NSString  *caption;  // retained; nil when the panel has no title
	NSFont    *font;     // retained; nil for the small system font
}
- (void)setContext:(GUIWIDGET*)ctx;
- (void)setCaption:(NSString*)text;
- (NSString*)caption;
// NSView has no font of its own, so the panel declares the two NSControl
// accessors by hand - which is what lets the typography code treat every
// widget the same way and keeps `panel/font-size` out of the shared layer
// as a special case.
- (void)setFont:(NSFont*)value;
- (NSFont*)font;
@end


// Reports `change` when the selection moves.
@interface RebolGuiPopUp : NSPopUpButton
{
	GUIWIDGET *context;
}
- (void)setContext:(GUIWIDGET*)ctx;
- (void)picked:(id)sender;
@end


// A text-list: a one-column table, which is its own data source and
// delegate. The strings live in `items`; the widget's handle is the scroll
// view around it, as an area's is.
@interface RebolGuiList : NSTableView <NSTableViewDataSource, NSTableViewDelegate>
{
	GUIWIDGET      *context;
	NSMutableArray *items;
	BOOL            quiet;   // a selection made by the script, not the user
}
- (void)setContext:(GUIWIDGET*)ctx;
- (NSMutableArray*)items;
- (void)setQuiet:(BOOL)on;
@end


// A date-field. Reports `change` when the user edits it - the action is
// not sent for setDateValue:, so a date set by the script is not reported
// back - and `focus`/`unfocus` as it gains and loses first responder.
@interface RebolGuiDatePicker : NSDatePicker
{
	GUIWIDGET *context;
}
- (void)setContext:(GUIWIDGET*)ctx;
- (void)picked:(id)sender;
@end


// Reports `change` while it is dragged. A progress bar needs no subclass -
// it is an NSProgressIndicator, which nobody interacts with.
@interface RebolGuiSlider : NSSlider
{
	GUIWIDGET *context;
	NSPoint    last;      // previous point of a drag, for the cell's hooks
	BOOL       pressing;  // between `down` and its `up`
}
- (void)setContext:(GUIWIDGET*)ctx;
- (void)moved:(id)sender;
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


/***********************************************************************
**  Where an event happened, in its window's client coordinates.
**
**  Every mouse event reports this, whichever view receives it: the
**  content view is flipped, so a point converted into it already has
**  the top-left origin and Y growing down that the rest of the extension
**  uses. A control's own view is usually NOT flipped, which is why no
**  event is measured in one.
***********************************************************************/
static NSPoint Client_Point(NSView *view, NSEvent *evt)
{
	NSView *content = [[view window] contentView];
	if (!content) content = view;
	return [content convertPoint:[evt locationInWindow] fromView:nil];
}


/***********************************************************************
**  The widget a `move` at this point is about: the DEEPEST one under
**  it, or NULL for the window's own background.
**
**  Walked through the window's widget list rather than asked of
**  hitTest:, for the reasons Widget_At_Point gives - hitTest: finds
**  field editors, scrollers and the text view inside an area, none of
**  which is a widget. One level at a time: the topmost direct child of
**  the window, then the topmost child of that, and so on.
**
**  A label is skipped, as if the pointer went through it: on Windows a
**  static control answers HTTRANSPARENT and its container gets the
**  mouse, so a caption sitting on an image leaves the image reporting.
**  A disabled control is skipped for the same reason - Win32 hands its
**  mouse input to the parent.
***********************************************************************/
static GUIWIDGET *Widget_Under_Point(GUIWIN *win, NSView *root, NSPoint inRoot)
{
	GUIWIDGET *found = NULL;
	GUIWIDGET *wid;
	REBOOL     deeper = TRUE;

	if (!win || !root) return NULL;

	while (deeper) {
		deeper = FALSE;
		// The list is in reverse creation order, so the first match at a
		// level is the topmost one there.
		for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next) {
			NSView *view = (NSView*)wid->handle;
			if ((GUIWIDGET*)wid->parent != found || !view || [view isHidden]) continue;
			if (wid->kind == W_GUI_WIDGET_TEXT) continue;
			if ([view isKindOfClass:[NSControl class]] && ![(NSControl*)view isEnabled]) continue;
			if (NSPointInRect([view convertPoint:inRoot fromView:root], [view bounds])) {
				found  = wid;
				deeper = TRUE;
				break;
			}
		}
	}
	return found;
}


/***********************************************************************
**  Whether a mouseMoved: that reached a window's content view is one
**  it should report, and where.
**
**  AppKit sends a tracking area's mouse-moved event to its owner AND
**  down the key window's responder chain - and the first responder of
**  every window here is its content view. So while one window is key,
**  moves over ANOTHER window arrive at the key one too, and the owner
**  can get the same event twice. Windows reports WM_MOUSEMOVE only to
**  the window under the pointer, once; that is the rule applied here.
***********************************************************************/
static BOOL Move_Point(NSView *content, NSEvent *evt, NSPoint *out)
{
	// The two copies are not necessarily the same NSEvent object, so they
	// are recognised by what they say: same moment, same place, same
	// window. Two real moves never share a timestamp.
	static NSTimeInterval last_time = -1;
	static NSPoint        last_at   = {0, 0};
	static NSInteger      last_win  = 0;
	NSWindow *win = [content window];

	if (!win) return NO;

	if ([evt timestamp] == last_time
	    && NSEqualPoints([evt locationInWindow], last_at)
	    && [[evt window] windowNumber] == last_win) return NO;
	last_time = [evt timestamp];
	last_at   = [evt locationInWindow];
	last_win  = [[evt window] windowNumber];

	if ([evt window]) {
		if ([evt window] != win) return NO;
		*out = [content convertPoint:[evt locationInWindow] fromView:nil];
	} else {
		// No window on the event (an inactive application can get these):
		// ask which window is under the pointer instead.
		NSPoint at = [NSEvent mouseLocation];
		if ([NSWindow windowNumberAtPoint:at belowWindowWithWindowNumber:0]
		    != [win windowNumber]) return NO;
		at   = [win convertRectFromScreen:NSMakeRect(at.x, at.y, 0, 0)].origin;
		*out = [content convertPoint:at fromView:nil];
	}
	return NSPointInRect(*out, [content bounds]);
}

/***********************************************************************
**  `down` / `up` on the pressable controls (button, check, radio, slider).
**
**  In window client coordinates, like every other mouse event.
***********************************************************************/
static void Queue_Press(NSView *view, GUIWIDGET *ctx, REBCNT type, NSEvent *evt, REBINT extra)
{
	NSPoint p;
	if (!ctx || !ctx->hob || !evt) return;
	p = Client_Point(view, evt);
	Gui_Queue_Event(ctx->hob, type, (REBINT)floor(p.x), (REBINT)floor(p.y),
	                Modifiers(evt) | extra);
}

@implementation RebolGuiButton

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

/***********************************************************************
**  A push button - and a toggle - as tall as its box.
**
**  NSBezelStyleRounded, the standard push button, is drawn at ONE
**  height for its control size, whatever the frame: a taller box got a
**  standard button sitting at the bottom of it. The square bezel
**  (renamed "flexible push" in newer SDKs) looks the same and fills its
**  frame, but at the standard height it is not quite the standard
**  button. So the style follows the height: rounded up to the standard
**  height, flexible above it - chosen again on every resize, so a
**  button can grow and shrink back.
**
**  A check and a radio have no bezel to stretch and are left alone.
***********************************************************************/
- (BOOL)stretches
{
	return context && (context->kind == W_GUI_WIDGET_BUTTON
	                || context->kind == W_GUI_WIDGET_TOGGLE);
}

- (void)fitBezelTo:(CGFloat)height
{
	NSBezelStyle want;

	if (![self stretches]) return;
	if (plain <= 0) {
		NSBezelStyle was = [self bezelStyle];
		[self setBezelStyle:NSBezelStyleRounded];
		plain = [[self cell] cellSize].height;
		[self setBezelStyle:was];
	}
	want = (height > plain + 1.0) ? NSBezelStyleRegularSquare : NSBezelStyleRounded;
	if ([self bezelStyle] != want) [self setBezelStyle:want];
}

- (void)setFrame:(NSRect)frame
{
	[self fitBezelTo:frame.size.height];
	[super setFrame:frame];
}

- (void)setFrameSize:(NSSize)size
{
	[self fitBezelTo:size.height];
	[super setFrameSize:size];
}

// What a zero size asks for: the STANDARD button, whatever style a tall
// frame has switched it to meanwhile.
- (NSSize)plainFittingSize
{
	NSBezelStyle was;
	NSSize       size;

	if (![self stretches]) return [self fittingSize];
	was = [self bezelStyle];
	[self setBezelStyle:NSBezelStyleRounded];
	size = [self fittingSize];
	[self setBezelStyle:was];
	return size;
}

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

/***********************************************************************
**  The press is tracked HERE rather than by NSButtonCell.
**
**  -[NSButton mouseDown:] runs a modal loop until the button comes up,
**  and Gui_Pump is inside [NSApp sendEvent:] for all of it - so a `down`
**  queued before calling super would reach Rebol only together with the
**  `up`. The same problem the slider has, solved the same way: without
**  super, dragged and up arrive as ordinary messages, one per pump.
**
**  That leaves this subclass doing what the loop did: the pressed look
**  (highlighted while the pointer is over it, as a native button does),
**  and on release inside, what a click means - a check toggles itself
**  here, a radio is switched on by the shared layer. `clicked:` is
**  called directly rather than through the target/action machinery, so
**  AppKit's own radio grouping is not involved.
**
**  Keyboard activation (Space, a key equivalent) still goes through
**  -performClick: and the action, and reports only `click`.
***********************************************************************/
- (void)mouseDown:(NSEvent*)evt
{
	if (![self isEnabled]) return;
	pressing = YES;
	inside   = YES;
	[self highlight:YES];
	Queue_Press(self, context, EVT_DOWN, evt,
	            [evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0);
}

- (void)mouseDragged:(NSEvent*)evt
{
	NSPoint p;
	BOOL    now;

	if (!pressing) return;
	Queue_Press(self, context, EVT_MOVE, evt, 0);

	p   = [self convertPoint:[evt locationInWindow] fromView:nil];
	now = NSPointInRect(p, [self bounds]);
	if (now != inside) {
		inside = now;
		[self highlight:now];
	}
}

- (void)mouseUp:(NSEvent*)evt
{
	NSPoint p;

	if (!pressing) return;
	pressing = NO;
	[self highlight:NO];

	// Asked again rather than trusted: there may have been no drag.
	p      = [self convertPoint:[evt locationInWindow] fromView:nil];
	inside = NSPointInRect(p, [self bounds]);

	Queue_Press(self, context, EVT_UP, evt, 0);

	if (inside) {
		// A check and a toggle keep a state of their own; a radio's is
		// settled by the shared layer.
		if (context && (context->kind == W_GUI_WIDGET_CHECK
		             || context->kind == W_GUI_WIDGET_TOGGLE)) [self setNextState];
		[self clicked:self];
	}
}

@end


/***********************************************************************
**  Everything a field does, for both field classes.
**
**  `queue:` - a notification has no cursor position, so, as with a
**  button, the position slot carries the control's own offset.
**
**  `accepted:` - ENTER, reported as a `click`: the same word a button
**  uses, because it is the same thing - the control was activated rather
**  than merely edited. Sending the action is also what stops AppKit
**  beeping at an Enter with nowhere to go, which is Win32's complaint too.
**
**  The three controlText... methods fire for USER edits only -
**  setStringValue: does not call them, so unlike Win32's EN_CHANGE there
**  is nothing to suppress when Rebol writes to the control.
**
**  No comments inside the macro: a line comment ending in the backslash
**  would swallow the line after it.
***********************************************************************/
#define TEXT_FIELD_BODY \
- (void)setContext:(GUIWIDGET*)ctx { context = ctx; } \
- (void)queue:(REBCNT)type \
{ \
	NSRect frame; \
	if (!context || !context->hob) return; \
	frame = [self frame]; \
	Gui_Queue_Event(context->hob, type, \
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y, \
	                Modifier_Bits([NSEvent modifierFlags])); \
} \
- (void)accepted:(id)sender { [self queue:EVT_CLICK]; } \
- (void)controlTextDidChange:(NSNotification*)note       { [self queue:EVT_CHANGE]; } \
- (void)controlTextDidBeginEditing:(NSNotification*)note { [self queue:EVT_FOCUS]; } \
- (void)controlTextDidEndEditing:(NSNotification*)note   { [self queue:EVT_UNFOCUS]; }

@implementation RebolGuiTextField
TEXT_FIELD_BODY
@end

@implementation RebolGuiSecureField
TEXT_FIELD_BODY
@end


@implementation RebolGuiPanel

// The only thing this class adds to a plain NSView, and the only reason it
// exists: children are positioned from the TOP-left, like every other
// offset in this extension.
- (BOOL)isFlipped { return YES; }

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (void)setCaption:(NSString*)text
{
	if (text == caption) return;
	[caption release];
	caption = [text retain];
	[self setNeedsDisplay:YES];
}

- (NSString*)caption { return caption; }

- (void)setFont:(NSFont*)value
{
	if (value == font) return;
	[font release];
	font = [value retain];
	[self setNeedsDisplay:YES];
}

- (NSFont*)font
{
	return font ? font : [NSFont systemFontOfSize:[NSFont smallSystemFontSize]];
}

- (void)dealloc
{
	[caption release];
	[font release];
	[super dealloc];
}

// No mouse handlers: a panel holds things, it does not report. Both
// Detach_Control() and the event code ask before sending, so their absence
// is fine.

/***********************************************************************
**  It draws the frame, and NOTHING ELSE - no background whatsoever.
**
**  That is a rule, not an implementation detail. An earlier version of
**  this class filled its bounds with windowBackgroundColor, and adding
**  one panel to a window made every widget already in that window
**  vanish. A container has nothing to say about the pixels it covers:
**  whatever is beneath it must show through, and an opaque fill turns
**  any mistake about a frame into a blank window rather than a
**  misplaced rectangle.
**
**  So the top line is drawn in two pieces with a gap for the caption,
**  rather than the usual trick of painting the background back over the
**  text. Breaking the line needs no colour and cannot cover anything.
***********************************************************************/
- (void)drawRect:(NSRect)dirty
{
	NSRect     bounds = [self bounds];
	NSRect     frame;
	CGFloat    inset  = 0.0;
	NSSize     size   = NSZeroSize;
	NSBezierPath *path;
	NSDictionary *attrs = nil;

	if (!context || !(context->state & GUI_PANEL_BORDER)) return;

	if (caption && [caption length] > 0) {
		// -font answers the small system font when none was set, so the
		// caption follows `panel/font` and `panel/font-size` with no
		// branch of its own. The colour likewise comes from the widget
		// context, which is where `panel/color` put it.
		attrs = @{
			NSFontAttributeName: [self font],
			NSForegroundColorAttributeName:
				(GUI_COLOR_HAS(context->color)
					? [NSColor colorWithSRGBRed:GUI_COLOR_R(context->color)/255.0
					                      green:GUI_COLOR_G(context->color)/255.0
					                       blue:GUI_COLOR_B(context->color)/255.0
					                      alpha:1.0]
					: [NSColor labelColor])
		};
		size  = [caption sizeWithAttributes:attrs];
		inset = (CGFloat)(int)(size.height / 2.0);
	}

	// Half-pixel offsets keep a one-pixel stroke on the pixel grid rather
	// than spread over two.
	frame = NSMakeRect(0.5, inset + 0.5,
	                   bounds.size.width - 1.0,
	                   bounds.size.height - inset - 1.0);
	if (frame.size.width <= 0.0 || frame.size.height <= 0.0) return;

	// tertiaryLabelColor rather than a fixed grey: it follows the system
	// appearance, so the frame is right in dark mode without asking.
	[[NSColor tertiaryLabelColor] set];
	path = [NSBezierPath bezierPath];
	[path setLineWidth:1.0];

	if (attrs) {
		CGFloat gap_x1 = (CGFloat)PANEL_CAPTION_X - 2.0;
		CGFloat gap_x2 = gap_x1 + size.width + 4.0;
		if (gap_x2 > NSMaxX(frame)) gap_x2 = NSMaxX(frame);

		// top line, in two pieces, with the caption sitting in the gap
		[path moveToPoint:NSMakePoint(NSMinX(frame), NSMinY(frame))];
		[path lineToPoint:NSMakePoint(gap_x1,        NSMinY(frame))];
		[path moveToPoint:NSMakePoint(gap_x2,        NSMinY(frame))];
		[path lineToPoint:NSMakePoint(NSMaxX(frame), NSMinY(frame))];
	} else {
		[path moveToPoint:NSMakePoint(NSMinX(frame), NSMinY(frame))];
		[path lineToPoint:NSMakePoint(NSMaxX(frame), NSMinY(frame))];
	}

	// ... and the other three sides
	[path moveToPoint:NSMakePoint(NSMaxX(frame), NSMinY(frame))];
	[path lineToPoint:NSMakePoint(NSMaxX(frame), NSMaxY(frame))];
	[path lineToPoint:NSMakePoint(NSMinX(frame), NSMaxY(frame))];
	[path lineToPoint:NSMakePoint(NSMinX(frame), NSMinY(frame))];
	[path stroke];

	if (attrs) {
		[caption drawAtPoint:NSMakePoint((CGFloat)PANEL_CAPTION_X, 0.0)
		      withAttributes:attrs];
	}
}

@end


@implementation RebolGuiPopUp

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (void)picked:(id)sender
{
	NSRect frame;
	if (!context || !context->hob) return;
	frame = [self frame];
	Gui_Queue_Event(context->hob, EVT_CHANGE,
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                Modifier_Bits([NSEvent modifierFlags]));
}

@end


@implementation RebolGuiDatePicker

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (void)queue:(REBCNT)type
{
	NSRect frame;
	if (!context || !context->hob) return;
	frame = [self frame];
	Gui_Queue_Event(context->hob, type,
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                Modifier_Bits([NSEvent modifierFlags]));
}

- (void)picked:(id)sender { [self queue:EVT_CHANGE]; }

// Return and Enter are a `click`, as in a field. NSDatePicker has no use
// for them and beeps; the key is consumed here instead of reaching it.
- (void)keyDown:(NSEvent*)evt
{
	NSString *chars = [evt charactersIgnoringModifiers];
	unichar   c     = [chars length] ? [chars characterAtIndex:0] : 0;
	if (c == '\r' || c == 3 /* keypad Enter */) {
		[self queue:EVT_CLICK];
		return;
	}
	[super keyDown:evt];
}

// Its intrinsic height is the height it is drawn for: a taller frame only
// stretches the bezel, and leaves the text sitting at the top of it. So
// the frame keeps its height and is centred in the box it was given.
- (void)setFrame:(NSRect)frame
{
	CGFloat want = [self intrinsicContentSize].height;
	if (want > 0 && frame.size.height > want) {
		frame.origin.y   += floor((frame.size.height - want) / 2.0);
		frame.size.height = want;
	}
	[super setFrame:frame];
}

- (BOOL)becomeFirstResponder
{
	BOOL ok = [super becomeFirstResponder];
	if (ok) [self queue:EVT_FOCUS];
	return ok;
}

- (BOOL)resignFirstResponder
{
	BOOL ok = [super resignFirstResponder];
	if (ok) [self queue:EVT_UNFOCUS];
	return ok;
}

@end


@implementation RebolGuiList

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }
- (void)setQuiet:(BOOL)on { quiet = on; }

// With `scrollable?` off, the wheel goes past the list to whatever holds
// its scroll view - as for a control which does not scroll - rather than
// to the scroll view itself.
- (void)scrollWheel:(NSEvent*)evt
{
	if (context && (context->state & GUI_LIST_FIXED)) {
		[[[self enclosingScrollView] nextResponder] scrollWheel:evt];
		return;
	}
	[super scrollWheel:evt];
}

- (NSMutableArray*)items
{
	if (!items) items = [[NSMutableArray alloc] init];
	return items;
}

- (void)dealloc
{
	[items release];
	[super dealloc];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)table
{
	return (NSInteger)[items count];
}

- (id)tableView:(NSTableView*)table objectValueForTableColumn:(NSTableColumn*)column
            row:(NSInteger)row
{
	return (row >= 0 && row < (NSInteger)[items count]) ? [items objectAtIndex:row] : nil;
}

// The widget's `color`, applied as each row is drawn - a cell-based table
// has no text colour of its own to set once.
- (void)tableView:(NSTableView*)table willDisplayCell:(id)cell
   forTableColumn:(NSTableColumn*)column row:(NSInteger)row
{
	NSColor *color = nil;
	if (context && GUI_COLOR_HAS(context->color)) {
		color = [NSColor colorWithSRGBRed:GUI_COLOR_R(context->color) / 255.0
		                            green:GUI_COLOR_G(context->color) / 255.0
		                             blue:GUI_COLOR_B(context->color) / 255.0
		                            alpha:1.0];
	}
	// A selected row keeps the system's own contrast.
	if ([cell respondsToSelector:@selector(setTextColor:)]) {
		[cell setTextColor:(color && ![table isRowSelected:row])
			? color : [NSColor controlTextColor]];
	}
}

// Rows are picked, not edited.
- (BOOL)tableView:(NSTableView*)table shouldEditTableColumn:(NSTableColumn*)column
              row:(NSInteger)row
{
	return NO;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note
{
	NSRect frame;
	if (quiet || !context || !context->hob) return;
	frame = [[self enclosingScrollView] frame];
	Gui_Queue_Event(context->hob, EVT_CHANGE,
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                Modifier_Bits([NSEvent modifierFlags]));
}

- (BOOL)becomeFirstResponder
{
	BOOL ok = [super becomeFirstResponder];
	if (ok && context && context->hob) Gui_Queue_Event(context->hob, EVT_FOCUS, 0, 0, 0);
	return ok;
}

- (BOOL)resignFirstResponder
{
	BOOL ok = [super resignFirstResponder];
	if (ok && context && context->hob) Gui_Queue_Event(context->hob, EVT_UNFOCUS, 0, 0, 0);
	return ok;
}

// The font is the column's cell's, and the rows follow its height -
// which is what lets `font-size` work on a list like on anything else.
- (void)setFont:(NSFont*)font
{
	NSTableColumn *column = [[self tableColumns] firstObject];
	if (!font) font = [NSFont systemFontOfSize:[NSFont systemFontSize]];
	[[column dataCell] setFont:font];
	[self setRowHeight:ceil([font ascender] - [font descender] + [font leading]) + 2.0];
	[self reloadData];
}

- (NSFont*)font
{
	return [[[[self tableColumns] firstObject] dataCell] font];
}

@end


@implementation RebolGuiSlider

- (void)setContext:(GUIWIDGET*)ctx { context = ctx; }

- (void)moved:(id)sender
{
	NSRect frame;
	if (!context || !context->hob) return;
	frame = [self frame];
	Gui_Queue_Event(context->hob, EVT_CHANGE,
	                (REBINT)frame.origin.x, (REBINT)frame.origin.y,
	                Modifier_Bits([NSEvent modifierFlags]));
}


/***********************************************************************
**  The drag is tracked HERE rather than by NSSliderCell.
**
**  -[NSSliderCell trackMouse:...] runs a MODAL LOOP: it takes every
**  mouse event itself until the button comes up. Gui_Pump is inside
**  [NSApp sendEvent:] for the whole of that, so nothing drains the
**  event queue, and a whole drag's worth of `change` events arrive in
**  one batch at the end. Win32's trackbar captures the mouse instead
**  and posts its notifications through the ordinary loop, so Rebol runs
**  between drag steps - which is the behaviour to match.
**
**  Not calling super means the events arrive as ordinary mouseDragged:
**  messages, one per pump, and a handler sees the slider move.
**
**  The knob's pressed look is normally set by that same loop, through
**  three NSCell hooks it calls as the drag goes on: startTrackingAt:,
**  continueTracking:at: and stopTracking:at:mouseIsUp:. They are called
**  here instead, one per event, so the cell sets whatever state it uses
**  for that look - which on current macOS is NOT the cell's highlighted
**  flag alone. The value is still worked out by -trackTo: AFTER each
**  hook, so the drag behaves the same whatever the hooks do.
**
**  The value is worked out rather than asked for, from the point in this
**  view's own coordinates. A vertical slider's minimum is at the BOTTOM -
**  the same end the Windows backend reports as 0% - but whether y grows
**  up or down here is the view's own business: NSSlider answers
**  isFlipped YES on current macOS, and assuming it did not put a click on
**  the knob at the mirror position, so the knob jumped to the opposite
**  end. So the view is asked, and a flipped y is measured from the bottom.
***********************************************************************/
- (void)trackTo:(NSEvent*)evt
{
	NSPoint p    = [self convertPoint:[evt locationInWindow] fromView:nil];
	NSRect  box  = [self bounds];
	CGFloat knob = [[self cell] knobThickness];
	CGFloat span, at;
	double  value;

	if ([self isVertical]) {
		CGFloat up = [self isFlipped] ? (NSMaxY(box) - p.y) : (p.y - NSMinY(box));
		span = box.size.height - knob;
		at   = up - (knob / 2.0);
	} else {
		span = box.size.width - knob;
		at   = p.x - (knob / 2.0);
	}

	value = (span > 0.0) ? (double)(at / span) : 0.0;
	if (value < 0.0) value = 0.0;
	else if (value > 1.0) value = 1.0;

	if (value == [self doubleValue]) return;  // no movement, no event

	[self setDoubleValue:value];
	[self moved:self];
}

- (void)mouseDown:(NSEvent*)evt
{
	NSPoint p = [self convertPoint:[evt locationInWindow] fromView:nil];

	// Tracking without super means the enabled check is ours as well.
	if (![self isEnabled]) return;
	pressing = YES;

	Queue_Press(self, context, EVT_DOWN, evt,
	            [evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0);

	[[self cell] setHighlighted:YES];
	[[self cell] startTrackingAt:p inView:self];
	last = p;

	[self trackTo:evt];
	[self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent*)evt
{
	NSPoint p = [self convertPoint:[evt locationInWindow] fromView:nil];

	if (!pressing) return;

	Queue_Press(self, context, EVT_MOVE, evt, 0);

	[[self cell] continueTracking:last at:p inView:self];
	last = p;

	[self trackTo:evt];
	[self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent*)evt
{
	NSPoint p = [self convertPoint:[evt locationInWindow] fromView:nil];

	if (!pressing) return;
	pressing = NO;

	[self trackTo:evt];

	[[self cell] stopTracking:last at:p inView:self mouseIsUp:YES];
	[[self cell] setHighlighted:NO];
	[self setNeedsDisplay:YES];

	// After the last `change`, so `up` is the end of the drag.
	Queue_Press(self, context, EVT_UP, evt, 0);
}

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

- (void)textDidChange:(NSNotification*)note       { [self queue:EVT_CHANGE]; }
- (void)textDidBeginEditing:(NSNotification*)note { [self queue:EVT_FOCUS]; }
- (void)textDidEndEditing:(NSNotification*)note   { [self queue:EVT_UNFOCUS]; }

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

// No tracking area of its own any more: plain moves are all reported by
// the window's content view, whose area covers the whole window and which
// works out the widget under the pointer (see Widget_Under_Point). Two
// areas meant two copies of every move over an image. The ivar stays, so
// that an area left from before is still removed.
- (void)updateTrackingAreas
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		[tracking release];
		tracking = nil;
	}
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

	// Nothing to show yet: draw NOTHING, and in particular do not fill.
	//
	// This used to paint the background colour over its own bounds, which
	// is what a view with nothing to draw is usually told to do - and it
	// took every widget created before it off the window, exactly as the
	// panel's background fill did. A view that draws nothing hides
	// nothing; an opaque fill in a view that should have had something to
	// say is how a missing image turns into missing buttons.
	if (!context || !Gui_Widget_Pixels(context, &bits, &w, &h)) {
		debug_print("GUI: image widget %p has no pixels to draw\n", (void*)context);
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
	} else {
		// The other way this widget can come out blank. Build with
		// USE_TRACES to see which of the two it was.
		debug_print("GUI: CGImageCreate failed for a %dx%d image\n", (int)w, (int)h);
	}

	CGDataProviderRelease(provider);
	CGColorSpaceRelease(space);
}

- (void)queue:(REBCNT)type from:(NSEvent*)evt extra:(REBINT)extra
{
	NSPoint pt;
	if (!context || !context->hob) return;
	pt = Client_Point(self, evt);   // the window's coordinates, not ours
	Gui_Queue_Event(context->hob, type,
	                (REBINT)floor(pt.x), (REBINT)floor(pt.y),
	                Modifiers(evt) | extra);
}

// A subview covers its part of the window, so the window's own view stops
// hearing about clicks there - these report them instead, with the widget
// as the source. Plain moves are the content view's to report (see its
// mouseMoved:), so one arriving here through the responder chain is not.
- (void)mouseMoved:(NSEvent*)evt { }
- (void)mouseDragged:(NSEvent*)evt      { [self queue:EVT_MOVE from:evt extra:0]; }
- (void)rightMouseDragged:(NSEvent*)evt { [self queue:EVT_MOVE from:evt extra:0]; }

- (void)mouseDown:(NSEvent*)evt
{
	[self queue:EVT_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)mouseUp:(NSEvent*)evt       { [self queue:EVT_UP from:evt extra:0]; }
- (void)rightMouseDown:(NSEvent*)evt{ [self queue:EVT_ALT_DOWN from:evt extra:0]; }
- (void)rightMouseUp:(NSEvent*)evt  { [self queue:EVT_ALT_UP from:evt extra:0]; }
- (void)otherMouseDown:(NSEvent*)evt{ [self queue:EVT_AUX_DOWN from:evt extra:0]; }
- (void)otherMouseUp:(NSEvent*)evt  { [self queue:EVT_AUX_UP from:evt extra:0]; }

@end


/***********************************************************************
**  Which widget is under a point.
**
**  NOT hitTest:, for two reasons. It takes its point in the
**  SUPERVIEW's coordinate system rather than the view's own, which is
**  easy to get wrong and silently finds the wrong control; and it sees
**  views which are not widgets at all - a field editor, a scroller, a
**  text view inside an area.
**
**  So the window's own widget list is walked instead, with the point
**  converted into each candidate's coordinates. `inRoot` is in the
**  coordinates of `root`, which is the content view.
**
**  Only DIRECT children of the window are considered, which is what
**  Win32's ChildWindowFromPointEx does: a drop on a label sitting on an
**  image reports the image, and a drop on a radio inside a panel
**  reports the panel. The list is in reverse creation order, so the
**  first match is the topmost.
***********************************************************************/
static GUIWIDGET *Widget_At_Point(GUIWIN *win, NSView *root, NSPoint inRoot)
{
	GUIWIDGET *wid;

	if (!win || !root) return NULL;

	for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next) {
		NSView *view = (NSView*)wid->handle;
		if (wid->parent || !view || [view isHidden]) continue;
		if (NSPointInRect([view convertPoint:inRoot fromView:root],
		                  [view bounds])) return wid;
	}
	return NULL;
}


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
// matching WM_MOUSEMOVE on Windows. Entered/exited as well, for `leave`
// when the pointer goes out of the window - see mouseExited: below.
- (void)updateTrackingAreas
{
	if (tracking) {
		[self removeTrackingArea:tracking];
		[tracking release];
	}
	tracking = [[NSTrackingArea alloc]
		initWithRect:[self bounds]
		     options:(NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited
		              | NSTrackingActiveAlways | NSTrackingInVisibleRect)
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

//-- appearance ---------------------------------------------------------------
// AppKit tells each view when its appearance changes (macOS 10.14 and
// later; never called before). Asked of the content view, because a
// window can be given an appearance of its own - so the answer is per
// window, which is why the event's source is the window. The controls
// restyle themselves; the event is for what the script painted.
- (void)viewDidChangeEffectiveAppearance
{
	// No call to super: NSView's own does nothing, and naming it would not
	// build against the 10.13 SDK, which does not declare it.
	if (context) Gui_Theme_Changed(context, Gui_Window_Dark(context));
}

//-- mouse motion -------------------------------------------------------------
// Cocoa reports movement with a button held as a drag rather than a move.
// Windows makes no such distinction (it captures the mouse instead), so all
// four are reported here as plain `move` events.

// Every plain move in the window is reported from here - over the window's
// own background and over every widget alike - because this view's
// tracking area covers the whole window and the widgets have none. The
// source is the widget under the pointer, the position the window's.
- (void)mouseMoved:(NSEvent*)evt
{
	NSPoint    pt;
	GUIWIDGET *wid;
	REBHOB    *source;

	if (!context || !context->hob) return;
	if (!Move_Point(self, evt, &pt)) return;

	wid    = Widget_Under_Point(context, self, pt);
	source = (wid && wid->hob) ? wid->hob : context->hob;
	Gui_Queue_Event(source, EVT_MOVE,
	                (REBINT)floor(pt.x), (REBINT)floor(pt.y),
	                Modifiers(evt));
}
- (void)mouseDragged:(NSEvent*)evt      { [self queue:EVT_MOVE from:evt extra:0]; }
- (void)rightMouseDragged:(NSEvent*)evt { [self queue:EVT_MOVE from:evt extra:0]; }
- (void)otherMouseDragged:(NSEvent*)evt { [self queue:EVT_MOVE from:evt extra:0]; }

// The pointer went out of the window. `enter` and `leave` are otherwise
// worked out from the moves (see Hover_To() in gui-commands.c); this is
// the one change a move cannot report, because the next move - if any -
// comes from something this program does not own. Only the content view
// has a tracking area, so moving onto a widget is not an exit.
//
// Not during a press: the view the button went down in keeps the pointer
// until it is up, and reports the drag wherever it goes.
- (void)mouseEntered:(NSEvent*)evt { }
- (void)mouseExited:(NSEvent*)evt
{
	if ([NSEvent pressedMouseButtons] != 0) return;
	Gui_Pointer_Left();
}

//-- buttons ------------------------------------------------------------------

- (void)mouseDown:(NSEvent*)evt
{
	[self queue:EVT_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)mouseUp:(NSEvent*)evt           { [self queue:EVT_UP from:evt extra:0]; }

- (void)rightMouseDown:(NSEvent*)evt
{
	[self queue:EVT_ALT_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)rightMouseUp:(NSEvent*)evt      { [self queue:EVT_ALT_UP from:evt extra:0]; }

- (void)otherMouseDown:(NSEvent*)evt
{
	[self queue:EVT_AUX_DOWN from:evt
	      extra:([evt clickCount] == 2 ? GUI_FLAG_DOUBLE : 0)];
}
- (void)otherMouseUp:(NSEvent*)evt      { [self queue:EVT_AUX_UP from:evt extra:0]; }

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
	Gui_Queue_Event(context->hob, EVT_SCROLL_LINE,
	                (REBINT)floor(pt.x), (REBINT)floor(pt.y), lines);
}

//-- window delegate ----------------------------------------------------------

// Reported only; the window stays open until Rebol calls `close-window`.
- (BOOL)windowShouldClose:(id)sender
{
	if (context && context->hob)
		Gui_Queue_Event(context->hob, EVT_CLOSE, 0, 0, 0);
	return NO;
}

/***********************************************************************
**  The menu bar belongs to the APPLICATION on macOS, not to a window.
**
**  So the rule is: whichever window is key owns the bar. A window with
**  a menu puts it up when it becomes key, and there is nothing to take
**  down - the next window to become key replaces it, and one which has
**  no menu of its own leaves the last one standing, exactly as an
**  ordinary Mac application behaves.
***********************************************************************/
- (void)windowDidBecomeKey:(NSNotification*)note
{
	if (context && context->menu)
		[NSApp setMainMenu:(NSMenu*)context->menu];
}

- (void)menuPicked:(id)sender
{
	if (context) Gui_Menu_Picked(context, (REBCNT)[(NSMenuItem*)sender tag]);
}

// The Quit item of the application menu, which macOS insists on having.
// It reports `close` rather than ending the process: this is an extension
// inside an interpreter, and terminating it would take the session with
// it. What "quit" then means is Rebol's decision, like any other close.
- (void)quitPicked:(id)sender
{
	if (context && context->hob)
		Gui_Queue_Event(context->hob, EVT_CLOSE, 0, 0, 0);
}

- (void)windowDidResize:(NSNotification*)note
{
	NSSize size;
	if (!context || !context->hob) return;
	size = [self bounds].size;
	Gui_Queue_Event(context->hob, EVT_RESIZE,
	                (REBINT)size.width, (REBINT)size.height, 0);
}


/***********************************************************************
**  Drag and drop.
**
**  Registered only once a script asks for it - see Gui_Window_Set_Drop.
**  AppKit hands over a pasteboard, so files and text cost the same here,
**  which is why macOS produces `drop-text` and Win32 does not.
**
**  The strings are copied out as UTF-8 and turned into Rebol values
**  later, in `poll-events`: this runs from the OS while it is tracking a
**  drag, which is exactly the kind of place the event queue's no-Rebol-
**  allocation rule exists for.
***********************************************************************/
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
	if (!context) return NSDragOperationNone;
	return NSDragOperationCopy;
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender
{
	return context ? YES : NO;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
	NSPasteboard *board;
	NSArray      *urls;
	NSString     *text;
	GUIDROPDATA  *data = NULL;
	NSPoint       where;
	REBHOB       *target;

	if (!context || !context->hob) return NO;
	board = [sender draggingPasteboard];

	// Where, in the content view's own coordinates - and on WHAT, because a
	// drop lands on whatever is under the pointer, the same rule a click
	// follows.
	where  = [self convertPoint:[sender draggingLocation] fromView:nil];
	target = context->hob;
	{
		GUIWIDGET *wid = Widget_At_Point(context, self, where);
		if (wid && wid->hob) target = wid->hob;
	}

	urls = [board readObjectsForClasses:@[[NSURL class]]
	                            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
	if (urls && [urls count] > 0) {
		data = Gui_Drop_Payload(GUI_DROP_FILES, (REBCNT)([urls count] * 160));
		if (!data) return NO;
		for (NSURL *url in urls) {
			const char *path = [[url path] UTF8String];
			if (path) Gui_Drop_Append(data, (const REBYTE*)path, (REBCNT)strlen(path));
		}
	}
	else if ((text = [board stringForType:NSPasteboardTypeString])) {
		const char *utf8 = [text UTF8String];
		if (!utf8) return NO;
		data = Gui_Drop_Payload(GUI_DROP_TEXT, (REBCNT)strlen(utf8) + 1);
		if (!data) return NO;
		Gui_Drop_Append(data, (const REBYTE*)utf8, (REBCNT)strlen(utf8));
	}
	else return NO;

	// The content view is flipped, so the converted point already has a
	// top-left origin - the same coordinates every other event reports.
	Gui_Queue_Drop(target, data, (REBINT)floor(where.x), (REBINT)floor(where.y));
	return YES;
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
                       const REBYTE *title, REBCNT title_len, REBCNT flags)
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

		// Borderless is a mask of its own, not the absence of bits: with
		// no title bar there is no close box and nothing to resize by.
		if (flags & GUI_WIN_BORDERLESS) {
			style = NSWindowStyleMaskBorderless;
		} else if (flags & GUI_WIN_FIXED) {
			style &= ~NSWindowStyleMaskResizable;
		}

		// The rect is the CONTENT rect, so the requested size is the usable
		// area - the same promise the Windows backend makes.
		window = [[RebolGuiWindow alloc] initWithContentRect:rect
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

		// One field, and the same call the accessor uses - see the note
		// in gui.h. Applied after the handle is stored, because that is
		// what it reads the window out of.
		if (flags & GUI_WIN_TRANSPARENT) {
			win->background = GUI_BG_CLEAR;
			Gui_Window_Set_Background(win);
		}
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

		// Before the content view goes: every menu item targets it, and
		// the bar is this window's own object to release.
		Gui_Menu_Free(win);

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


//== menu bar =================================================================
//
// The asymmetry with Windows worth knowing about: a Win32 menu bar belongs
// to a window and sits inside it, while on macOS there is ONE bar, at the
// top of the screen, belonging to the application. This backend gives each
// window its own NSMenu and installs it when that window becomes key - see
// -windowDidBecomeKey: above - which is how a Mac application with several
// differently-menued windows behaves anyway.
//
// The other macOS rule is that the FIRST menu is the application menu: it
// is drawn in bold under the process name and is where Quit belongs. One
// is inserted here whether the caller asked for it or not, because the
// alternative is the caller's first menu silently becoming it.

static void Add_App_Menu(GUIWIN *win, NSMenu *bar)
{
	NSString   *name = [[NSProcessInfo processInfo] processName];
	NSMenuItem *slot = [[NSMenuItem alloc] initWithTitle:name
	                                             action:NULL
	                                      keyEquivalent:@""];
	NSMenu     *menu = [[NSMenu alloc] initWithTitle:name];
	NSMenuItem *quit;

	// Off, so that an item is enabled because this extension says so and
	// not because AppKit found a responder willing to take its action.
	[menu setAutoenablesItems:NO];

	quit = [[NSMenuItem alloc]
		initWithTitle:[@"Quit " stringByAppendingString:name]
		       action:@selector(quitPicked:)
		keyEquivalent:@"q"];
	[quit setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
	[quit setTarget:(id)[NSWINDOW_OF(win) contentView]];
	[menu addItem:quit];
	[quit release];

	[slot setSubmenu:menu];
	[bar addItem:slot];
	[menu release];
	[slot release];
}


REBOOL Gui_Menu_Begin(GUIWIN *win)
{
	@autoreleasepool {
		NSMenu *bar;

		if (!win || !win->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		bar = [[NSMenu alloc] initWithTitle:@"MainMenu"];
		if (!bar) return FALSE;
		[bar setAutoenablesItems:NO];

		Add_App_Menu(win, bar);

		win->menu = (void*)bar; // retained; released by Gui_Menu_Free()
		return TRUE;
	}
}


void* Gui_Menu_Add_Popup(GUIWIN *win, void *parent,
                         const REBYTE *label, REBCNT len)
{
	@autoreleasepool {
		NSString   *title;
		NSMenuItem *slot;
		NSMenu     *menu;

		if (!win || !win->menu) return NULL;
		title = To_NSString(label, len);
		if (!title) title = @"";

		slot = [[NSMenuItem alloc] initWithTitle:title
		                                  action:NULL
		                           keyEquivalent:@""];
		menu = [[NSMenu alloc] initWithTitle:title];
		[menu setAutoenablesItems:NO];
		[slot setSubmenu:menu];

		[(parent ? (NSMenu*)parent : (NSMenu*)win->menu) addItem:slot];
		[slot release];

		// The NSMenu is retained by the item, which is retained by the
		// menu it was just added to - so the whole tree hangs off the bar
		// and one release in Gui_Menu_Free() takes all of it.
		[menu autorelease];
		return (void*)menu;
	}
}


void Gui_Menu_Add_Item(GUIWIN *win, void *parent,
                       const REBYTE *label, REBCNT len, REBCNT item_id,
                       REBCNT key, REBCNT mods)
{
	@autoreleasepool {
		NSString   *title;
		NSString   *equiv = @"";
		NSMenuItem *item;

		if (!win || !win->menu) return;
		title = To_NSString(label, len);
		if (!title) title = @"";

		// A key equivalent is a property of the item here - there is no
		// accelerator table to keep in step, and no text to append: the
		// menu draws the shortcut itself.
		if (key) {
			unichar ch = (unichar)((key >= 'A' && key <= 'Z') ? key + 32 : key);
			equiv = [NSString stringWithCharacters:&ch length:1];
		}

		item = [[NSMenuItem alloc] initWithTitle:title
		                                  action:@selector(menuPicked:)
		                           keyEquivalent:equiv];
		if (key) {
			NSEventModifierFlags flags = NSEventModifierFlagCommand;
			if (mods & GUI_FLAG_SHIFT)   flags |= NSEventModifierFlagShift;
			if (mods & GUI_FLAG_CONTROL) flags |= NSEventModifierFlagControl;
			if (mods & GUI_FLAG_ALT)     flags |= NSEventModifierFlagOption;
			[item setKeyEquivalentModifierMask:flags];
		}
		[item setTag:(NSInteger)item_id];
		[item setTarget:(id)[NSWINDOW_OF(win) contentView]];
		[item setEnabled:YES];

		[(parent ? (NSMenu*)parent : (NSMenu*)win->menu) addItem:item];
		[item release];
	}
}


void Gui_Menu_Add_Separator(GUIWIN *win, void *parent)
{
	@autoreleasepool {
		if (!win || !win->menu) return;
		[(parent ? (NSMenu*)parent : (NSMenu*)win->menu)
			addItem:[NSMenuItem separatorItem]];
	}
}


REBOOL Gui_Menu_End(GUIWIN *win)
{
	@autoreleasepool {
		if (!win || !win->handle || !win->menu) return FALSE;

		// Up straight away if this window is the one in front; otherwise
		// it waits for the window to become key. Nothing here takes client
		// area from the window, so unlike Windows there is no layout to
		// put back.
		if ([NSWINDOW_OF(win) isKeyWindow] || ![NSApp mainMenu])
			[NSApp setMainMenu:(NSMenu*)win->menu];

		return TRUE;
	}
}


void Gui_Menu_Free(GUIWIN *win)
{
	@autoreleasepool {
		if (!win || !win->menu) return;

		// Only if it is still the one on screen: another window may have
		// taken the bar since, and pulling it out from under that one is
		// not this window's business.
		if ([NSApp mainMenu] == (NSMenu*)win->menu)
			[NSApp setMainMenu:[[[NSMenu alloc] initWithTitle:@""] autorelease]];

		[(NSMenu*)win->menu release];
		win->menu = NULL;
	}
}


// Depth first, because an id may name an item in any submenu, and NSMenu
// only searches the level it was asked about.
static NSMenuItem* Item_With_Tag(NSMenu *menu, NSInteger tag)
{
	for (NSMenuItem *item in [menu itemArray]) {
		if ([item tag] == tag && ![item hasSubmenu]) return item;
		if ([item hasSubmenu]) {
			NSMenuItem *found = Item_With_Tag([item submenu], tag);
			if (found) return found;
		}
	}
	return nil;
}


void Gui_Menu_Enable(GUIWIN *win, REBCNT item_id, REBOOL enabled)
{
	@autoreleasepool {
		NSMenuItem *item;
		if (!win || !win->menu) return;
		item = Item_With_Tag((NSMenu*)win->menu, (NSInteger)item_id);
		[item setEnabled:(enabled ? YES : NO)];
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
			Display_Pending = TRUE;
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
REBCNT Gui_Pump(void)
{
	REBCNT dispatched = 0;

	@autoreleasepool {
		NSEvent *evt;

		if (![NSThread isMainThread]) return 0;

		while ((evt = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                 untilDate:[NSDate distantPast]
		                                    inMode:NSDefaultRunLoopMode
		                                   dequeue:YES]))
		{
			[NSApp sendEvent:evt];
			dispatched++;
		}
		[NSApp updateWindows];

		// And the drawing - ALL of it, and only here.
		//
		// NSApplication's own -run loop displays dirty views between
		// events; this extension never calls -run, so nothing else would
		// ever do it. This is the one place where AppKit is in a state to
		// draw, which is why no other function in this file displays
		// anything - see the note on Display_Pending.
		//
		// A full -display rather than -displayIfNeeded: whatever else may
		// have happened to a view's needs-display flag, everything on the
		// window is painted after something changed. In a quiet poll -
		// the common case, several times a second - nothing is drawn at
		// all.
		if (Display_Pending) {
			NSArray *windows = [NSApp windows];
			NSUInteger n, count = [windows count];

			Display_Pending = FALSE;
			for (n = 0; n < count; n++) {
				NSWindow *w = (NSWindow*)[windows objectAtIndex:n];
				if ([w isVisible]) [w display];
			}
		}
	}

	return dispatched;
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


/***********************************************************************
**  The frame.
**
**  Read from the window's own styleMask, never shadowed.
**
**  Changing the mask keeps the FRAME rect, so the content rect gains or
**  loses whatever the title bar was taking - and everything in the
**  window is laid out in the content rect. So the content size is taken
**  first and given back afterwards, which is the same thing the Windows
**  backend does by measuring the client area.
***********************************************************************/
/***********************************************************************
**  The shadow is not a property of its own - it is the window's `border?`.
**  A titled window always casts one. Without a title bar, a borderless
**  NSWindow draws a thin outline exactly when it has a shadow, so the
**  two come and go together - which is what `border?` means - and nothing
**  else about the window changes. Worked out again after every change of
**  style mask or background, so a title bar taken away leaves the window
**  as `border?` says, however it got there.
***********************************************************************/
static void Update_Shadow(GUIWIN *win)
{
	NSWindow *window;
	BOOL      want;

	if (!win || !win->handle) return;
	window = NSWINDOW_OF(win);
	want = (([window styleMask] & NSWindowStyleMaskTitled)
	        || (win->flags & GUIW_BORDER)) ? YES : NO;
	if ([window hasShadow] != want) {
		[window setHasShadow:want];
		[window invalidateShadow];
	}
}

void Gui_Window_Apply_Border(GUIWIN *win)
{
	@autoreleasepool { Update_Shadow(win); }
}

static REBOOL Set_Window_Style_Mask(GUIWIN *win, NSUInteger mask)
{
	@autoreleasepool {
		NSWindow *window;
		NSRect    content;

		if (!win || !win->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		window = NSWINDOW_OF(win);
		if ([window styleMask] == mask) return TRUE;

		content = [window contentRectForFrameRect:[window frame]];
		[window setStyleMask:mask];
		[window setFrame:[window frameRectForContentRect:content] display:YES];
		Update_Shadow(win);

		// Losing the title bar takes the first responder with it often
		// enough to be worth putting back - a window with no key view has
		// no field editor, and typing goes nowhere.
		[window makeFirstResponder:[window contentView]];
		return TRUE;
	}
}


REBOOL Gui_Get_Resizable(GUIWIN *win)
{
	@autoreleasepool {
		if (!win || !win->handle) return FALSE;
		return ([NSWINDOW_OF(win) styleMask] & NSWindowStyleMaskResizable)
			? TRUE : FALSE;
	}
}


REBOOL Gui_Set_Resizable(GUIWIN *win, REBOOL on)
{
	@autoreleasepool {
		NSUInteger mask;
		if (!win || !win->handle) return FALSE;
		mask = [NSWINDOW_OF(win) styleMask];
		// Borderless is the absence of every bit, so setting one here would
		// quietly give the window a title bar back.
		if (mask == NSWindowStyleMaskBorderless) return FALSE;
		if (on) mask |=  NSWindowStyleMaskResizable;
		else    mask &= ~NSWindowStyleMaskResizable;
		return Set_Window_Style_Mask(win, mask);
	}
}


REBOOL Gui_Get_Title_Bar(GUIWIN *win)
{
	@autoreleasepool {
		if (!win || !win->handle) return FALSE;
		return ([NSWINDOW_OF(win) styleMask] & NSWindowStyleMaskTitled)
			? TRUE : FALSE;
	}
}


REBOOL Gui_Set_Title_Bar(GUIWIN *win, REBOOL on)
{
	if (!on) return Set_Window_Style_Mask(win, NSWindowStyleMaskBorderless);

	// Coming back from borderless: a title bar and the buttons on it. Not
	// resizable - whether it can be is its own property, and one this
	// window may never have had.
	return Set_Window_Style_Mask(win, NSWindowStyleMaskTitled
	                                | NSWindowStyleMaskClosable
	                                | NSWindowStyleMaskMiniaturizable);
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
#define NSPOPUP_OF(wid)  ((RebolGuiPopUp*)((wid)->handle))

// What a new control is added to: the panel holding it, or the window's
// content view. `wid->parent` is set before any creation call.
static NSView* Parent_View(GUIWIDGET *wid, GUIWIN *owner)
{
	if (wid->parent && ((GUIWIDGET*)wid->parent)->handle)
		return (NSView*)((GUIWIDGET*)wid->parent)->handle;
	return [NSWINDOW_OF(owner) contentView];
}


// An area's handle is its scroll view; the text itself lives one level in.
static NSTextView* Text_View_Of(GUIWIDGET *wid)
{
	if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_AREA) return nil;
	return (NSTextView*)[(NSScrollView*)wid->handle documentView];
}

// ... and a text-list's is too; the table lives one level in.
static RebolGuiList* List_View_Of(GUIWIDGET *wid)
{
	if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_TEXT_LIST) return nil;
	return (RebolGuiList*)[(NSScrollView*)wid->handle documentView];
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
	} else if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
		RebolGuiList *list = List_View_Of(wid);
		if (list) {
			[list setDelegate:nil];
			[list setDataSource:nil];
			[list setContext:NULL];
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

		content = Parent_View(wid, owner);
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
		case W_GUI_WIDGET_TOGGLE:
			// A push button which stays pushed: the on state is drawn in
			// the accent colour, as a pressed-in button would be.
			[button setButtonType:NSButtonTypePushOnPushOff];
			[button setBezelStyle:NSBezelStyleRounded];
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
		// Now that it knows its kind, the bezel can follow the height.
		[button setFrame:[button frame]];

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

		content = Parent_View(wid, owner);
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
			// `/secure` is its own class - see RebolGuiSecureField. Typed as
			// the plain one below, since everything this file calls on a
			// field is NSTextField's, which both inherit.
			RebolGuiTextField *field =
				(wid->kind == W_GUI_WIDGET_FIELD && (wid->state & GUI_TEXT_SECURE))
				? (RebolGuiTextField*)[[RebolGuiSecureField alloc] initWithFrame:rect]
				: [[RebolGuiTextField alloc] initWithFrame:rect];
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
				// Enter sends the action - see -accepted: above. A label
				// gets none: it is not editable and cannot be activated.
				[field setTarget:field];
				[field setAction:@selector(accepted:)];
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

		content = Parent_View(wid, owner);
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


/***********************************************************************
**  Marks a control for drawing. Does NOT draw it.
**
**  Nothing outside Gui_Pump() draws - see the note on Display_Pending.
**  Marking here and painting there is the whole of the arrangement: a
**  widget appears at the next poll, which for any program with an event
**  loop is immediately, and for one which never polls is never - as it
**  would be for any other AppKit program that does not run its loop.
***********************************************************************/
void Gui_Widget_Redraw(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE; // drawn by the next Gui_Pump()
	}
}


// The same thing here, and deliberately so: AppKit is only in a state to
// draw between events, so this backend has never painted anywhere but in
// Gui_Pump(). The distinction the two names carry is a Win32 one; keeping
// them apart is what lets that backend honour it.
void Gui_Widget_Invalidate(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE;
	}
}


void Gui_Window_Redraw(GUIWIN *win)
{
	@autoreleasepool {
		NSView *content;
		if (!win || !win->handle) return;
		content = [NSWINDOW_OF(win) contentView];
		[content setNeedsDisplay:YES];
		Display_Pending = TRUE; // drawn by the next Gui_Pump()
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
		Display_Pending = TRUE; // the hole it left has to be repainted
	}
}


/***********************************************************************
**  Nothing to convert on this side: AppKit already works in points,
**  which is what gui.h means by a logical unit. The scale is reported
**  only so that a caller can size an IMAGE to the device pixels behind
**  those points - 2.0 on a Retina display.
***********************************************************************/
REBDEC Gui_Get_Scale(GUIWIN *win)
{
	@autoreleasepool {
		NSWindow *window;
		NSScreen *screen = nil;

		if (win && win->handle) {
			window = NSWINDOW_OF(win);
			screen = [window screen];        // nil while off screen
			if (!screen) return (REBDEC)[window backingScaleFactor];
		}
		if (!screen) screen = [NSScreen mainScreen];
		return screen ? (REBDEC)[screen backingScaleFactor] : 1.0;
	}
}


//== screens ==================================================================
//
// A display is named by its CGDirectDisplayID, which stays the same while
// the display is connected - an NSScreen object is not kept, because AppKit
// replaces them when the configuration changes. Every question walks
// [NSScreen screens] again and matches by id.
//
// Index 0 of that array is the screen with the menu bar: the primary one,
// and the origin of the global space - which is why Screen_Height() flips
// against it, here as for windows, so that a screen's offset is in the
// same top-left, Y-down space as a window's.

static CGDirectDisplayID Display_Id(NSScreen *screen)
{
	NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
	return num ? (CGDirectDisplayID)[num unsignedIntValue] : 0;
}

static void Display_Key(NSScreen *screen, REBYTE *key)
{
	snprintf((char*)key, GUI_SCREEN_KEY, "%u", (unsigned)Display_Id(screen));
}

static NSScreen *Find_Screen(const REBYTE *key, REBOOL *primary)
{
	NSArray   *screens = [NSScreen screens];
	REBYTE     k[GUI_SCREEN_KEY];
	NSUInteger n;

	for (n = 0; n < [screens count]; n++) {
		NSScreen *screen = [screens objectAtIndex:n];
		Display_Key(screen, k);
		if (strcmp((const char*)k, (const char*)key) == 0) {
			if (primary) *primary = (n == 0);
			return screen;
		}
	}
	return nil;
}

// A Cocoa rectangle (bottom-left origin, Y up) as a top-left one, Y down.
static void Flip_Rect(NSRect r, REBINT *x, REBINT *y, REBINT *w, REBINT *h)
{
	*x = (REBINT)r.origin.x;
	*y = (REBINT)(Screen_Height() - (r.origin.y + r.size.height));
	*w = (REBINT)r.size.width;
	*h = (REBINT)r.size.height;
}

REBCNT Gui_Screen_Keys(REBYTE (*keys)[GUI_SCREEN_KEY], REBCNT max)
{
	@autoreleasepool {
		NSArray   *screens = [NSScreen screens];
		NSUInteger n, count = [screens count];

		// Already primary first: AppKit puts the menu-bar screen at 0.
		for (n = 0; n < count && n < max; n++)
			Display_Key([screens objectAtIndex:n], keys[n]);
		return (REBCNT)count;
	}
}

REBOOL Gui_Screen_Info(const REBYTE *key, GUISCREENINFO *info)
{
	@autoreleasepool {
		REBOOL    primary = FALSE;
		NSScreen *screen;

		if (!key || !info || !(screen = Find_Screen(key, &primary))) return FALSE;

		Flip_Rect([screen frame],        &info->x,  &info->y,  &info->w,  &info->h);
		// Without the menu bar and the Dock - where a window can go.
		Flip_Rect([screen visibleFrame], &info->wx, &info->wy, &info->ww, &info->wh);
		info->scale   = (REBDEC)[screen backingScaleFactor];
		info->primary = primary;
		return TRUE;
	}
}

// `localizedName` is macOS 10.15 and later; the SDK floor here is 10.13,
// so it is asked for by selector. Before 10.15 there is no public API for
// the name at all, so a display is called by its id.
REBSER* Gui_Screen_Name(const REBYTE *key)
{
	@autoreleasepool {
		NSScreen *screen;
		NSString *name = nil;

		if (!key || !(screen = Find_Screen(key, NULL))) return NULL;
		if ([screen respondsToSelector:@selector(localizedName)])
			name = [screen performSelector:@selector(localizedName)];
		if (!name) name = [NSString stringWithFormat:@"Display %s", (const char*)key];
		return From_NSString(name);
	}
}

// Where the pointer is on the desktop, for `track-mouse`. See gui.h.
REBOOL Gui_Pointer_At(REBYTE *key, REBINT *x, REBINT *y, REBINT *mods, REBOOL *ours)
{
	@autoreleasepool {
		NSPoint    at = [NSEvent mouseLocation];   // global, Y up
		NSArray   *screens = [NSScreen screens];
		NSScreen  *screen = nil;
		NSInteger  number;
		NSUInteger n;
		NSRect     frame;

		*ours = FALSE;

		// A press which started in one of our windows is still that
		// window's while the button is held: the view the button went
		// down in gets the drags, wherever the pointer goes.
		if ([NSApp isActive] && [NSEvent pressedMouseButtons] != 0
		    && [NSApp keyWindow] != nil) {
			*ours = TRUE;
			return TRUE;
		}

		// The TOPMOST window at that point, of any application; one of
		// ours reports its own moves.
		number = [NSWindow windowNumberAtPoint:at belowWindowWithWindowNumber:0];
		if (number > 0 && [NSApp windowWithWindowNumber:number] != nil) {
			*ours = TRUE;
			return TRUE;
		}

		for (n = 0; n < [screens count]; n++) {
			NSScreen *s = [screens objectAtIndex:n];
			if (NSMouseInRect(at, [s frame], NO)) { screen = s; break; }
		}
		if (!screen && [screens count] > 0) screen = [screens objectAtIndex:0];
		if (!screen) return FALSE;

		Display_Key(screen, key);
		frame = [screen frame];
		// From the screen's top-left corner, Y down.
		*x = (REBINT)floor(at.x - frame.origin.x);
		*y = (REBINT)floor(frame.origin.y + frame.size.height - at.y);
		*mods = Modifier_Bits([NSEvent modifierFlags]);
		return TRUE;
	}
}

// Whether the window is drawn in the dark appearance. The API is 10.14 and
// later and the SDK floor is 10.13, so it is asked for by selector and by
// the appearance's NAME rather than the NSAppearanceNameDarkAqua symbol.
// Before 10.14 there is no dark appearance to be in.
// Nothing to do: AppKit's controls and default colours follow the
// appearance by themselves, so `dark-controls?` only matters on Windows.
void Gui_Window_Dark_Controls(GUIWIN *win, REBOOL on)
{
	(void)win; (void)on;
}

REBOOL Gui_Window_Dark(GUIWIN *win)
{
	@autoreleasepool {
		NSView       *view;
		NSAppearance *look;
		NSString     *match;
		SEL           best = NSSelectorFromString(@"bestMatchFromAppearancesWithNames:");

		if (!win || !win->handle) return FALSE;
		view = [NSWINDOW_OF(win) contentView];
		if (!view || ![view respondsToSelector:@selector(effectiveAppearance)]) return FALSE;
		look = [view performSelector:@selector(effectiveAppearance)];
		if (!look || ![look respondsToSelector:best]) return FALSE;
		match = [look performSelector:best
		                   withObject:@[@"NSAppearanceNameAqua", @"NSAppearanceNameDarkAqua"]];
		return [match isEqualToString:@"NSAppearanceNameDarkAqua"] ? TRUE : FALSE;
	}
}

REBOOL Gui_Window_Screen(GUIWIN *win, REBYTE *key)
{
	@autoreleasepool {
		NSScreen *screen;

		if (!win || !win->handle || !key) return FALSE;
		// The screen holding most of the window; nil while it is on none
		// (hidden, or moved entirely off every display).
		screen = [NSWINDOW_OF(win) screen];
		if (!screen) return FALSE;
		Display_Key(screen, key);
		return TRUE;
	}
}


REBOOL Gui_Widget_Natural_Size(GUIWIDGET *wid, REBINT *w, REBINT *h)
{
	@autoreleasepool {
		NSSize   size;
		NSFont  *font;
		CGFloat  line;

		if (!wid || !wid->handle) return FALSE;

		switch (wid->kind) {
		case W_GUI_WIDGET_DATE_FIELD: {
			/***************************************************************
			**  Measured with the WIDEST date it can show, not the one it
			**  shows now: fitting to today leaves no room for a longer
			**  date set later, and a few pixels short clips the text. Two
			**  digit day and month, and an evening time for a 12-hour
			**  clock's "PM". Rounded UP, with a pixel of slack, as the
			**  stepper's width is fractional.
			***************************************************************/
			NSDatePicker *picker = (NSDatePicker*)wid->handle;
			NSDate *was = [[picker dateValue] retain];
			NSDateComponents *c = [[[NSDateComponents alloc] init] autorelease];
			NSDate *wide;
			[c setYear:2000]; [c setMonth:12]; [c setDay:28];
			[c setHour:20]; [c setMinute:58]; [c setSecond:58];
			wide = [[NSCalendar currentCalendar] dateFromComponents:c];
			if (wide) [picker setDateValue:wide];
			size = [picker fittingSize];
			[picker setDateValue:was];
			[was release];
			if (w) *w = (REBINT)ceil(size.width) + 2;
			if (h) *h = (REBINT)ceil(size.height);
			return TRUE; }

		case W_GUI_WIDGET_BUTTON:
		case W_GUI_WIDGET_TOGGLE:
			size = [NSBUTTON_OF(wid) plainFittingSize];
			break;

		case W_GUI_WIDGET_CHECK:
		case W_GUI_WIDGET_RADIO:
		case W_GUI_WIDGET_TEXT:
		case W_GUI_WIDGET_FIELD:
		case W_GUI_WIDGET_DROP_DOWN:
			// AppKit measures its own controls, bezel and all, which is a
			// better answer than any padding table this file could keep.
			size = [(NSControl*)wid->handle fittingSize];
			break;

		case W_GUI_WIDGET_AREA: {
			// A scroll view fits to nothing useful - it is happy at any
			// size - so the text view's font decides, and four lines is
			// the smallest thing that reads as multi-line.
			font = [Text_View_Of(wid) font];
			if (!font) font = [NSFont systemFontOfSize:[NSFont systemFontSize]];
			line = [font ascender] - [font descender] + [font leading];
			size = NSMakeSize(0.0, line * 4.0 + 8.0);
			break; }

		case W_GUI_WIDGET_TEXT_LIST: {
			// Six rows, and the bezel round them.
			RebolGuiList *list = List_View_Of(wid);
			size = NSMakeSize(0.0, ([list rowHeight] + [list intercellSpacing].height) * 6.0 + 4.0);
			break; }

		default:
			return FALSE; // no text, so no natural size to give
		}

		// A control with no title fits to almost nothing; an entry the
		// user is meant to type into wants room whatever is in it now.
		if (wid->kind == W_GUI_WIDGET_FIELD
		 || wid->kind == W_GUI_WIDGET_AREA
		 || wid->kind == W_GUI_WIDGET_TEXT_LIST
		 || wid->kind == W_GUI_WIDGET_DROP_DOWN) {
			CGFloat least = 20.0 * [NSFont systemFontSize] * 0.55;
			if (size.width < least) size.width = least;
		}

		if (w) *w = (REBINT)(size.width  + 0.5);
		if (h) *h = (REBINT)(size.height + 0.5);
		return TRUE;
	}
}


/***********************************************************************
**  Tooltips: NSView's own `toolTip`, so AppKit does the delay, the
**  placement and the look. Read back from the view, like the font.
**
**  An area is two views - the scroll view which is the widget, and the
**  text view filling it - and a tip belongs to whichever view is under
**  the pointer, which is the text view. Both get it.
***********************************************************************/
REBOOL Gui_Widget_Set_Tip(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	@autoreleasepool {
		NSString *tip = nil;
		if (!wid || !wid->handle) return FALSE;
		if (utf8 && len > 0) {
			tip = To_NSString(utf8, len);
			if (!tip) return FALSE;
		}
		[NSVIEW_OF(wid) setToolTip:tip];
		if (wid->kind == W_GUI_WIDGET_AREA) [Text_View_Of(wid) setToolTip:tip];
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST) [List_View_Of(wid) setToolTip:tip];
		return TRUE;
	}
}

REBSER* Gui_Widget_Get_Tip(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSString *tip;
		if (!wid || !wid->handle) return NULL;
		tip = [NSVIEW_OF(wid) toolTip];
		if (!tip || [tip length] == 0) return NULL;
		return From_NSString(tip);
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
		case W_GUI_WIDGET_DROP_DOWN:
			// An NSPopUpButton's own `title` is not what it displays.
			return From_NSString([NSPOPUP_OF(wid) titleOfSelectedItem]);
		case W_GUI_WIDGET_PANEL:
			// A panel is a plain view; its caption is the extension's own.
			return From_NSString([NSPANEL_OF(wid) caption]);
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
		case W_GUI_WIDGET_PANEL:
			[NSPANEL_OF(wid) setCaption:value];
			break;
		default:
			[NSBUTTON_OF(wid) setTitle:value];
			// Setting a plain title drops the attributes, colour and all,
			// so a coloured button has to be dressed again.
			Apply_Button_Color(wid);
			break;
		}
		return TRUE;
	}
}


//== typography ===============================================================

// Which object actually carries the font. Everything here answers -font and
// -setFont: - NSControl declares both, the text view has its own, and the
// panel declares them by hand - so one lookup serves every kind.
static id Font_Target(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return nil;
	if (wid->kind == W_GUI_WIDGET_AREA) return (id)Text_View_Of(wid);
	if (wid->kind == W_GUI_WIDGET_TEXT_LIST) return (id)List_View_Of(wid);
	return (id)wid->handle;
}


REBOOL Gui_Widget_Get_Font(GUIWIDGET *wid, REBSER **name, REBINT *size,
                           REBCNT *style)
{
	@autoreleasepool {
		id      target = Font_Target(wid);
		NSFont *font;
		NSFontTraitMask traits;

		if (name)  *name  = NULL;
		if (size)  *size  = 0;
		if (style) *style = 0;

		if (![target respondsToSelector:@selector(font)]) return FALSE;
		font = [target font];
		if (!font) return FALSE;

		// The FAMILY, not the font name: "Georgia" rather than
		// "Georgia-BoldItalic", so that reading a font back and setting it
		// on something else does not carry a style along with it.
		if (name) *name = From_NSString([font familyName]);
		if (size) *size = (REBINT)([font pointSize] + 0.5);
		if (style) {
			traits = [[NSFontManager sharedFontManager] traitsOfFont:font];
			if (traits & NSBoldFontMask)   *style |= GUI_FONT_BOLD;
			if (traits & NSItalicFontMask) *style |= GUI_FONT_ITALIC;
		}
		return TRUE;
	}
}


REBOOL Gui_Widget_Set_Font(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len,
                           REBINT size, REBCNT style)
{
	@autoreleasepool {
		id       target = Font_Target(wid);
		NSFontManager *manager = [NSFontManager sharedFontManager];
		NSString *family = To_NSString(utf8, len);
		NSFont   *font = nil;
		CGFloat   points = (size > 0) ? (CGFloat)size : [NSFont systemFontSize];
		NSFontTraitMask mask = 0;

		if (![target respondsToSelector:@selector(setFont:)]) return FALSE;

		if (style & GUI_FONT_BOLD)   mask |= NSBoldFontMask;
		if (style & GUI_FONT_ITALIC) mask |= NSItalicFontMask;

		if (family && [family length] > 0) {
			font = [manager fontWithFamily:family traits:0 weight:5 size:points];
			// A name which is a face rather than a family - "Georgia-Bold" -
			// still resolves this way, so both are accepted.
			if (!font) font = [NSFont fontWithName:family size:points];
		}
		if (!font) font = [NSFont systemFontOfSize:points];
		if (!font) return FALSE;

		// convertFont: hands back what it was given when a family has no
		// such face, so a missing italic is a plain font rather than none.
		if (mask) {
			NSFont *styled = [manager convertFont:font toHaveTrait:mask];
			if (styled) font = styled;
		}

		[target setFont:font];

		// A button's colour lives in its attributed title, which carries
		// the font as one of its attributes - so changing the font means
		// building that string again.
		Apply_Button_Color(wid);

		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE;
		return TRUE;
	}
}


/***********************************************************************
**  The colour a widget draws its text in.
**
**  AppKit has no one way to say this: a text field and a text view take
**  a colour directly, while a button, a check, a radio and a pop-up
**  have none at all and are coloured by giving them an ATTRIBUTED
**  title. The panel draws its own caption and simply reads the widget
**  context at paint time.
***********************************************************************/
static NSColor* Widget_Color(GUIWIDGET *wid)
{
	if (!wid || !GUI_COLOR_HAS(wid->color)) return nil;
	return [NSColor colorWithSRGBRed:GUI_COLOR_R(wid->color) / 255.0
	                           green:GUI_COLOR_G(wid->color) / 255.0
	                            blue:GUI_COLOR_B(wid->color) / 255.0
	                           alpha:1.0];
}

// wid->background as a colour: nil when there is none to paint, which is
// both "the platform's own" and "transparent" - the two differ in what
// drawsBackground is set to, not in the colour.
static NSColor* Widget_Background(GUIWIDGET *wid)
{
	if (!wid || !GUI_COLOR_HAS(wid->background)) return nil;
	return [NSColor colorWithSRGBRed:GUI_COLOR_R(wid->background) / 255.0
	                           green:GUI_COLOR_G(wid->background) / 255.0
	                            blue:GUI_COLOR_B(wid->background) / 255.0
	                           alpha:1.0];
}

static void Apply_Button_Color(GUIWIDGET *wid)
{
	NSButton *button;
	NSColor  *color;
	NSString *title;

	if (!wid || !wid->handle) return;

	// Only these three. A pop-up's title is whichever menu item is
	// selected rather than a title of its own, so an attributed title
	// would fight the menu; everything else here has no title at all.
	switch (wid->kind) {
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
	case W_GUI_WIDGET_TOGGLE:
		break;
	default:
		return;
	}

	button = NSBUTTON_OF(wid);
	color  = Widget_Color(wid);
	title  = [button title];
	if (!title) return;

	if (!color) {
		// Setting the plain title is what REMOVES the attributes; there is
		// no "no attributed title" to set.
		[button setTitle:title];
		return;
	}

	[button setAttributedTitle:
		[[[NSAttributedString alloc] initWithString:title attributes:@{
			NSFontAttributeName: [button font],
			NSForegroundColorAttributeName: color
		}] autorelease]];
}


REBOOL Gui_Widget_Set_Color(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSColor *color;

		if (!wid || !wid->handle) return FALSE;
		color = Widget_Color(wid);

		switch (wid->kind) {
		case W_GUI_WIDGET_AREA:
			[Text_View_Of(wid) setTextColor:
				(color ? color : [NSColor textColor])];
			break;

		case W_GUI_WIDGET_TEXT:
			[(NSTextField*)wid->handle setTextColor:
				(color ? color : [NSColor labelColor])];
			break;

		case W_GUI_WIDGET_FIELD:
			[(NSTextField*)wid->handle setTextColor:
				(color ? color : [NSColor textColor])];
			break;

		case W_GUI_WIDGET_PANEL:
			// Read straight out of the widget context by -drawRect:.
			break;

		case W_GUI_WIDGET_TEXT_LIST:
			// Read by the table's willDisplayCell: as each row is drawn.
			[List_View_Of(wid) setNeedsDisplay:YES];
			break;

		case W_GUI_WIDGET_DROP_DOWN:
			// See Apply_Button_Color: a pop-up shows a menu item, not a
			// title of its own, so this is the one control here whose
			// colour AppKit will not take.
			return FALSE;

		default:
			Apply_Button_Color(wid);
			break;
		}

		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE;
		return TRUE;
	}
}


/***********************************************************************
**  wid->background, applied.
**
**  Far less work than the Win32 side, and for a structural reason: a
**  view here is composited into its superview, so a control which draws
**  no background of its own already has its parent's pixels underneath.
**  Nothing has to fetch them, and a label over an image widget needs no
**  more than switching its own fill off.
**
**  A check and a radio draw no background to begin with, so only an
**  explicit colour needs doing there - through the layer, since an
**  NSButton has no background colour of its own.
***********************************************************************/
void Gui_Widget_Set_Background(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSColor *color;
		REBOOL   clear;

		if (!wid || !wid->handle) return;
		color = Widget_Background(wid);
		clear = GUI_BG_IS_CLEAR(wid->background);

		switch (wid->kind) {
		case W_GUI_WIDGET_TEXT: {
			NSTextField *label = (NSTextField*)wid->handle;
			// A label starts unbezeled and already draws its background,
			// so all three states are this one pair of properties.
			[label setDrawsBackground:(clear ? NO : YES)];
			[label setBackgroundColor:
				(color ? color : [NSColor controlColor])];
			break; }

		case W_GUI_WIDGET_TEXT_LIST:
			// Like an entry: a colour is honoured, transparency is not.
			[List_View_Of(wid) setBackgroundColor:
				(color ? color : [NSColor controlBackgroundColor])];
			[List_View_Of(wid) setNeedsDisplay:YES];
			break;

		case W_GUI_WIDGET_AREA:
		case W_GUI_WIDGET_FIELD: {
			// An entry has a bezel and a background that belong together;
			// a colour is honoured, transparency is not, because what is
			// left is a bezel around nothing.
			NSTextField *field = (NSTextField*)wid->handle;
			if (wid->kind == W_GUI_WIDGET_AREA) {
				NSTextView *text = Text_View_Of(wid);
				[text setDrawsBackground:(color ? YES : NO)];
				if (color) [text setBackgroundColor:color];
			} else if (color) {
				[field setBackgroundColor:color];
			} else {
				[field setBackgroundColor:[NSColor textBackgroundColor]];
			}
			break; }

		default: {
			// A check, a radio and a panel draw no background of their
			// own, so transparency is what they already are and only a
			// colour needs doing. Through the LAYER, deliberately: a
			// layer's background is composited UNDER the subviews, where
			// filling in -drawRect: has hidden widgets in this file
			// before.
			NSView *view = NSVIEW_OF(wid);
			if (color) {
				[view setWantsLayer:YES];
				[[view layer] setBackgroundColor:[color CGColor]];
			} else if ([view layer]) {
				[[view layer] setBackgroundColor:NULL];
			}
			break; }
		}

		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE;
	}
}


/***********************************************************************
**  win->background, applied.
**
**  Two properties, and AppKit does the rest: the content view draws no
**  background of its own, so the window's colour IS the client area and
**  a window which is not opaque with a clear colour is see-through.
**
**  No colour key and no layer surgery - the compositor already deals in
**  alpha, which is the one place this backend has less to do than the
**  Win32 one rather than more.
***********************************************************************/
/***********************************************************************
**  Whether the window accepts drops.
**
**  Registering is per view, and only the content view is registered:
**  the controls are not dragging destinations, so a drop anywhere over
**  the window arrives here and performDragOperation: works out which
**  widget was under the pointer.
**
**  Both file URLs and plain text are asked for, which is why macOS
**  reports `drop-text` as well as `drop-file` - on Win32 that would
**  need a registered IDropTarget and is not implemented.
***********************************************************************/
/***********************************************************************
**  The keyboard focus, which AppKit calls the first responder.
**
**  Two things make this less direct than SetFocus. A text field being
**  edited is not itself the first responder - the window's shared field
**  EDITOR is, with the field as its delegate - so asking has to look
**  through it. And an area's handle is the scroll view, not the text
**  view inside it, which is the thing that can actually be responded to.
**
**  makeFirstResponder: answers NO for a view which refuses the role -
**  a label, a progress indicator - which is exactly the answer wanted,
**  so it is passed straight through.
***********************************************************************/
static NSView *Focus_Target(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return nil;
	if (wid->kind == W_GUI_WIDGET_AREA) return (NSView*)Text_View_Of(wid);
	if (wid->kind == W_GUI_WIDGET_TEXT_LIST) return (NSView*)List_View_Of(wid);
	return (NSView*)wid->handle;
}


REBOOL Gui_Window_Set_Focus(GUIWIN *win)
{
	@autoreleasepool {
		if (!win || !win->handle) return FALSE;
		[NSWINDOW_OF(win) makeKeyAndOrderFront:nil];
		return TRUE;
	}
}


REBOOL Gui_Widget_Set_Focus(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSView   *view = Focus_Target(wid);
		NSWindow *window;

		if (!view || !wid->owner || !wid->owner->handle) return FALSE;
		window = NSWINDOW_OF(wid->owner);

		// Focusing a control in a window nobody is looking at is not what
		// the caller means; both platforms bring the window forward.
		[window makeKeyAndOrderFront:nil];
		return [window makeFirstResponder:view] ? TRUE : FALSE;
	}
}


REBOOL Gui_Widget_Has_Focus(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSView     *view = Focus_Target(wid);
		NSWindow   *window;
		NSResponder *first;

		if (!view || !wid->owner || !wid->owner->handle) return FALSE;
		window = NSWINDOW_OF(wid->owner);
		first  = [window firstResponder];

		if (first == (NSResponder*)view) return TRUE;

		// The field editor stands in for whichever text field is being
		// edited, and names it as its delegate.
		if ([first isKindOfClass:[NSText class]]
		    && (NSView*)[(NSText*)first delegate] == view) return TRUE;

		return FALSE;
	}
}


void Gui_Window_Set_Drop(GUIWIN *win, REBOOL accept)
{
	@autoreleasepool {
		if (!win) return;
		if (win->handle) {
			NSView *view = [NSWINDOW_OF(win) contentView];
			if (accept) {
				[view registerForDraggedTypes:@[
					NSPasteboardTypeFileURL,
					NSPasteboardTypeString
				]];
			} else {
				[view unregisterDraggedTypes];
			}
		}
		if (accept) win->flags |=  GUIW_ACCEPTS_DROP;
		else        win->flags &= ~GUIW_ACCEPTS_DROP;
	}
}


void Gui_Window_Set_Background(GUIWIN *win)
{
	@autoreleasepool {
		NSWindow *window;
		NSColor  *color;

		if (!win || !win->handle) return;
		window = NSWINDOW_OF(win);

		if (GUI_BG_IS_CLEAR(win->background)) {
			[window setOpaque:NO];
			[window setBackgroundColor:[NSColor clearColor]];
			Update_Shadow(win);
			return;
		}

		[window setOpaque:YES];
		Update_Shadow(win);

		if (GUI_COLOR_HAS(win->background)) {
			color = [NSColor colorWithSRGBRed:GUI_COLOR_R(win->background) / 255.0
			                           green:GUI_COLOR_G(win->background) / 255.0
			                            blue:GUI_COLOR_B(win->background) / 255.0
			                           alpha:1.0];
		} else {
			color = [NSColor windowBackgroundColor];
		}
		[window setBackgroundColor:color];
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
		Display_Pending = TRUE; // moved, so the old place needs repainting
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


//-- panel --------------------------------------------------------------------

REBOOL Gui_Create_Panel(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h,
                        const REBYTE *text, REBCNT len)
{
	@autoreleasepool {
		RebolGuiPanel *panel;
		NSView *content;

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = Parent_View(wid, owner);
		if (!content) return FALSE;

		// Deliberately not an NSBox, which is how AppKit usually draws a
		// captioned frame: an NSBox puts its children in a contentView of
		// its own and insets them by an amount it chooses, which would
		// move everything a panel holds the moment it grew an edge, and
		// by a different amount than Windows. A plain flipped view which
		// strokes its own frame keeps a child's offset meaning the same
		// thing on both platforms, framed or not.
		panel = [[RebolGuiPanel alloc] initWithFrame:
			NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)];
		if (!panel) return FALSE;

		[panel setContext:wid];
		if (text && len > 0) [panel setCaption:To_NSString(text, len)];

		[content addSubview:panel];
		wid->handle = (void*)panel;
		return TRUE;
	}
}


void Gui_Panel_Border_Changed(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		if (![NSThread isMainThread]) return;

		// Marked, not displayed - Gui_Pump() does the drawing, for the
		// reason given where Display_Pending is declared.
		[NSPANEL_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE;
	}
}


//-- drop-down ----------------------------------------------------------------

REBOOL Gui_Create_Drop_Down(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	@autoreleasepool {
		RebolGuiPopUp *popup;
		NSView *content;

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = Parent_View(wid, owner);
		if (!content) return FALSE;

		// pullsDown:NO makes it a chooser rather than a menu button. Unlike
		// Win32's combo box, the frame is the closed control and the list
		// is drawn outside it - no room has to be reserved.
		popup = [[RebolGuiPopUp alloc]
			initWithFrame:NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)
			    pullsDown:NO];
		if (!popup) return FALSE;

		[popup setContext:wid];
		[popup setTarget:popup];
		[popup setAction:@selector(picked:)];

		[content addSubview:popup];
		wid->handle = (void*)popup;
		return TRUE;
	}
}


//-- date-field ---------------------------------------------------------------

REBOOL Gui_Create_Date_Field(GUIWIDGET *wid, GUIWIN *owner,
                             REBINT x, REBINT y, REBINT w, REBINT h)
{
	@autoreleasepool {
		RebolGuiDatePicker *picker;
		NSView *content;
		NSDatePickerElementFlags parts = NSDatePickerElementFlagYearMonthDay;

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = Parent_View(wid, owner);
		if (!content) return FALSE;

		picker = [[RebolGuiDatePicker alloc] initWithFrame:
			NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h)];
		if (!picker) return FALSE;

		// The field with the little stepper - the compact form, and the
		// nearest thing to Windows' date picker, which drops a calendar
		// instead.
		if (wid->state & GUI_DATE_TIME) parts |= NSDatePickerElementFlagHourMinute;
		[picker setDatePickerStyle:NSDatePickerStyleTextFieldAndStepper];
		[picker setDatePickerElements:parts];
		[picker setDatePickerMode:NSDatePickerModeSingle];
		[picker setBezeled:YES];
		[picker setDrawsBackground:YES];
		// The regular control size and system font - the same as a field.
		// NSDatePicker defaults to neither, and its text then looks smaller
		// and set lower than the fields next to it.
		[picker setControlSize:NSControlSizeRegular];
		[picker setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
		[picker setFrame:[picker frame]];   // clamp to its height, now known
		// Local time, the same as the Windows control; the calendar is the
		// user's own.
		[picker setTimeZone:[NSTimeZone localTimeZone]];
		[picker setCalendar:[NSCalendar currentCalendar]];
		[picker setDateValue:[NSDate date]];

		[picker setContext:wid];
		[picker setTarget:picker];
		[picker setAction:@selector(picked:)];

		[content addSubview:picker];
		wid->handle = (void*)picker;
		return TRUE;
	}
}


REBOOL Gui_Widget_Get_Date(GUIWIDGET *wid, GUIDATE *out)
{
	@autoreleasepool {
		NSDateComponents *c;
		NSCalendar *cal;

		if (!wid || !wid->handle || !out) return FALSE;
		cal = [NSCalendar currentCalendar];
		[cal setTimeZone:[NSTimeZone localTimeZone]];
		c = [cal components:(NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay
		                     | NSCalendarUnitHour | NSCalendarUnitMinute | NSCalendarUnitSecond)
		           fromDate:[(NSDatePicker*)wid->handle dateValue]];
		if (!c) return FALSE;
		out->year  = (REBINT)[c year];
		out->month = (REBINT)[c month];
		out->day   = (REBINT)[c day];
		out->ns    = (wid->state & GUI_DATE_TIME)
			? ((REBI64)[c hour] * 3600 + [c minute] * 60 + [c second]) * 1000000000
			: 0;
		return TRUE;
	}
}


void Gui_Widget_Set_Date(GUIWIDGET *wid, const GUIDATE *in)
{
	@autoreleasepool {
		NSDateComponents *c;
		NSCalendar *cal;
		NSDate *date;
		REBI64 s;

		if (!wid || !wid->handle || !in) return;
		cal = [NSCalendar currentCalendar];
		[cal setTimeZone:[NSTimeZone localTimeZone]];
		c = [[[NSDateComponents alloc] init] autorelease];
		[c setYear:in->year];
		[c setMonth:in->month];
		[c setDay:in->day];
		s = (wid->state & GUI_DATE_TIME) ? in->ns / 1000000000 : 0;
		[c setHour:(NSInteger)(s / 3600)];
		[c setMinute:(NSInteger)((s / 60) % 60)];
		[c setSecond:(NSInteger)(s % 60)];
		// NSCalendar rolls an impossible date over (30-Feb becomes early
		// March); Windows refuses one. Refused here too, for one answer.
		date = [cal dateFromComponents:c];
		if (!date) return;
		{	NSDateComponents *back = [cal components:(NSCalendarUnitYear
			    | NSCalendarUnitMonth | NSCalendarUnitDay) fromDate:date];
			if ([back day] != in->day || [back month] != in->month) return;
		}
		[(NSDatePicker*)wid->handle setDateValue:date];
		Display_Pending = TRUE;
	}
}


//-- text-list ----------------------------------------------------------------

REBOOL Gui_Create_Text_List(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	@autoreleasepool {
		NSScrollView  *scroll;
		RebolGuiList  *list;
		NSTableColumn *column;
		NSView        *content;
		NSRect rect = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = Parent_View(wid, owner);
		if (!content) return FALSE;

		scroll = [[NSScrollView alloc] initWithFrame:rect];
		if (!scroll) return FALSE;
		[scroll setBorderType:NSBezelBorder];
		// Shown only while the rows do not fit.
		[scroll setHasVerticalScroller:YES];
		[scroll setHasHorizontalScroller:NO];
		[scroll setAutohidesScrollers:YES];

		list = [[RebolGuiList alloc] initWithFrame:
			NSMakeRect(0, 0, [scroll contentSize].width, [scroll contentSize].height)];
		if (!list) { [scroll release]; return FALSE; }

		column = [[[NSTableColumn alloc] initWithIdentifier:@"item"] autorelease];
		[column setEditable:NO];
		[column setResizingMask:NSTableColumnAutoresizingMask];
		[list addTableColumn:column];
		[list setHeaderView:nil];
		[list setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
		[list setAllowsEmptySelection:YES];
		[list setAllowsMultipleSelection:NO];
		[list setFont:nil];   // the system font, and the row height to match
		[list setContext:wid];
		[list setDataSource:list];
		[list setDelegate:list];
		[column setWidth:[scroll contentSize].width];

		[scroll setDocumentView:list];
		[list release];   // the scroll view holds it now

		[content addSubview:scroll];
		wid->handle = (void*)scroll;
		return TRUE;
	}
}


REBCNT Gui_Widget_Count_Items(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return 0;
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST)
			return (REBCNT)[[List_View_Of(wid) items] count];
		return (REBCNT)[NSPOPUP_OF(wid) numberOfItems];
	}
}


REBSER* Gui_Widget_Get_Item(GUIWIDGET *wid, REBCNT n)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return NULL;
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			NSMutableArray *items = [List_View_Of(wid) items];
			if (n >= (REBCNT)[items count]) return NULL;
			return From_NSString([items objectAtIndex:(NSUInteger)n]);
		}
		if ((NSInteger)n >= [NSPOPUP_OF(wid) numberOfItems]) return NULL;
		return From_NSString([NSPOPUP_OF(wid) itemTitleAtIndex:(NSInteger)n]);
	}
}


REBOOL Gui_Widget_Add_Item(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	@autoreleasepool {
		NSString *title;

		if (!wid || !wid->handle) return FALSE;
		title = To_NSString(utf8, len);
		if (!title) title = @"";

		if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			RebolGuiList *list = List_View_Of(wid);
			[[list items] addObject:title];
			[list setQuiet:YES];
			[list noteNumberOfRowsChanged];
			[list setQuiet:NO];
			return TRUE;
		}

		// Titles are a menu's identity to AppKit, which would drop a repeat;
		// a list of strings is data here, so duplicates have to survive.
		[[NSPOPUP_OF(wid) menu] addItem:
			[[[NSMenuItem alloc] initWithTitle:title action:NULL keyEquivalent:@""]
				autorelease]];
		return TRUE;
	}
}


void Gui_Widget_Clear_Items(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			RebolGuiList *list = List_View_Of(wid);
			[list setQuiet:YES];
			[[list items] removeAllObjects];
			[list deselectAll:nil];
			[list reloadData];
			[list setQuiet:NO];
			return;
		}
		[NSPOPUP_OF(wid) removeAllItems];
	}
}


REBINT Gui_Widget_Get_Index(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return -1;
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST)
			return (REBINT)[List_View_Of(wid) selectedRow]; // -1 for none
		return (REBINT)[NSPOPUP_OF(wid) indexOfSelectedItem];
	}
}


void Gui_Widget_Set_Index(GUIWIDGET *wid, REBINT n)
{
	@autoreleasepool {
		RebolGuiPopUp *popup;

		if (!wid || !wid->handle) return;
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			RebolGuiList *list = List_View_Of(wid);
			// Quiet, so a pick made by the script is not reported back as
			// the user's - as on Windows, where LB_SETCURSEL notifies no one.
			[list setQuiet:YES];
			if (n < 0 || n >= [list numberOfRows]) {
				[list deselectAll:nil];
			} else {
				[list selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)n]
				  byExtendingSelection:NO];
				[list scrollRowToVisible:(NSInteger)n];
			}
			[list setQuiet:NO];
			return;
		}
		popup = NSPOPUP_OF(wid);

		if (n < 0 || n >= [popup numberOfItems]) {
			// AppKit has no "select nothing", so the selected item is
			// simply deselected through the menu.
			[popup selectItem:nil];
			return;
		}
		[popup selectItemAtIndex:(NSInteger)n];
	}
}


REBOOL Gui_Create_Range_Control(GUIWIDGET *wid, GUIWIN *owner,
                                REBINT x, REBINT y, REBINT w, REBINT h)
{
	@autoreleasepool {
		NSView *content;
		NSRect  rect = NSMakeRect((CGFloat)x, (CGFloat)y, (CGFloat)w, (CGFloat)h);

		if (!wid || !owner || !owner->handle) return FALSE;
		if (![NSThread isMainThread]) return FALSE;

		content = Parent_View(wid, owner);
		if (!content) return FALSE;

		if (wid->kind == W_GUI_WIDGET_SLIDER) {
			RebolGuiSlider *slider = [[RebolGuiSlider alloc] initWithFrame:rect];
			if (!slider) return FALSE;

			[slider setMinValue:0.0];
			[slider setMaxValue:1.0];
			[slider setDoubleValue:0.0];
			// Taller than wide means upright - the same rule the Windows
			// backend applies, so a caller need not know either platform.
			// A vertical NSSlider already has its minimum at the bottom.
			if (h > w) [slider setVertical:YES];
			// Reports while it is dragged, not only when it is let go.
			// This covers the keyboard and any programmatic action; the
			// MOUSE is tracked by the subclass - see -trackTo: for why.
			[slider setContinuous:YES];

			[slider setContext:wid];
			[slider setTarget:slider];
			[slider setAction:@selector(moved:)];

			[content addSubview:slider];
			wid->handle = (void*)slider;
			return TRUE;
		}

		{
			NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:rect];
			if (!bar) return FALSE;

			// Bar is the default style, and saying so explicitly would
			// mean picking between two spellings of the constant which
			// changed name across SDKs.
			[bar setIndeterminate:NO];
			[bar setMinValue:0.0];
			[bar setMaxValue:1.0];
			[bar setDoubleValue:0.0];

			[content addSubview:bar];
			wid->handle = (void*)bar;
			return TRUE;
		}
	}
}


REBDEC Gui_Widget_Get_Value(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return 0.0;
		if (wid->kind == W_GUI_WIDGET_SLIDER)
			return (REBDEC)[(NSSlider*)wid->handle doubleValue];
		return (REBDEC)[(NSProgressIndicator*)wid->handle doubleValue];
	}
}


void Gui_Widget_Set_Value(GUIWIDGET *wid, REBDEC value)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		if (wid->kind == W_GUI_WIDGET_SLIDER) {
			[(NSSlider*)wid->handle setDoubleValue:(double)value];
		} else {
			[(NSProgressIndicator*)wid->handle setDoubleValue:(double)value];
		}
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
		NSControlStateValue want;

		if (!wid || !wid->handle) return;
		button = NSBUTTON_OF(wid);

		// Already there: nothing to do. The radio grouping above this file
		// re-asserts every radio in the window on every click, and a
		// control asked to become what it already is still redraws - and,
		// where a theme animates the change, animates it again.
		want = on ? NSControlStateValueOn : NSControlStateValueOff;
		if ([button state] == want) return;

		action = [button action];
		[button setAction:NULL];
		[button setState:want];
		[button setAction:action];
	}
}


REBOOL Gui_Widget_Get_Enabled(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		// A scroll view has no enabled state, so an area answers with
		// whether its text can be SELECTED - editability is the separate
		// read-only question, and asking that here would report a
		// read-only log as disabled.
		if (wid->kind == W_GUI_WIDGET_AREA)
			return [Text_View_Of(wid) isSelectable] ? TRUE : FALSE;
		if (wid->kind == W_GUI_WIDGET_TEXT_LIST)
			return [List_View_Of(wid) isEnabled] ? TRUE : FALSE;
		return [(NSControl*)wid->handle isEnabled] ? TRUE : FALSE;
	}
}


// Enabled and read-only are one property on a text view and two
// questions, so both callers write the combination rather than half of
// it. Disabling an area and enabling it again leaves it read-only if
// that is what it was.
static void Apply_Text_Editability(GUIWIDGET *wid, REBOOL enabled)
{
	REBOOL writable = (enabled && !(wid->state & GUI_TEXT_READ_ONLY))
	                ? TRUE : FALSE;

	if (wid->kind == W_GUI_WIDGET_AREA) {
		NSTextView *text = Text_View_Of(wid);
		[text setEditable:(writable ? YES : NO)];
		[text setSelectable:(enabled ? YES : NO)];
	} else {
		[(NSTextField*)wid->handle setEditable:(writable ? YES : NO)];
		[(NSTextField*)wid->handle setSelectable:YES];
	}
}


REBOOL Gui_Widget_Set_Enabled(GUIWIDGET *wid, REBOOL enabled)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		if (wid->kind == W_GUI_WIDGET_AREA) {
			Apply_Text_Editability(wid, enabled);
		} else if (wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			[List_View_Of(wid) setEnabled:(enabled ? YES : NO)];
		} else {
			[(NSControl*)wid->handle setEnabled:(enabled ? YES : NO)];
			// A field has both: NSControl's enabled state, and the
			// editability that read-only also writes.
			if (wid->kind == W_GUI_WIDGET_FIELD)
				Apply_Text_Editability(wid, enabled);
		}
		return TRUE;
	}
}


/***********************************************************************
**  Scrolling. The area's handle IS the scroll view, so the clip view's
**  bounds against the document's frame is the whole of it.
**
**  A text view is flipped, so y grows downward and 0 is the top - the
**  same direction the fraction runs in, and the same direction the
**  Win32 line numbers run in.
***********************************************************************/
static REBOOL Scroll_Metrics_Of(GUIWIDGET *wid, NSScrollView **sv,
                                CGFloat *span, CGFloat *at)
{
	NSClipView *clip;
	NSView     *doc;

	if (!wid || !wid->handle) return FALSE;
	if (wid->kind != W_GUI_WIDGET_AREA && wid->kind != W_GUI_WIDGET_TEXT_LIST) return FALSE;

	*sv  = (NSScrollView*)wid->handle;
	clip = [*sv contentView];
	doc  = [*sv documentView];
	if (!clip || !doc) return FALSE;

	*span = [doc frame].size.height - [clip bounds].size.height;
	*at   = [clip bounds].origin.y;
	return TRUE;
}


// The scroll view keeps scrolling from code either way; only the bar and
// the wheel (see -[RebolGuiList scrollWheel:]) go.
void Gui_Widget_Set_Scrollable(GUIWIDGET *wid, REBOOL on)
{
	@autoreleasepool {
		if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_TEXT_LIST) return;
		[(NSScrollView*)wid->handle setHasVerticalScroller:(on ? YES : NO)];
		Display_Pending = TRUE;
	}
}


/***********************************************************************
**  The border: a field's bezel, or the scroll view's border for an area
**  and a text-list. A field without its bezel stops drawing its
**  background with it, so it is asked to go on drawing one - otherwise
**  `background` would have nothing to show.
***********************************************************************/
REBOOL Gui_Widget_Get_Border(GUIWIDGET *wid)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		if (wid->kind == W_GUI_WIDGET_FIELD)
			return [(NSTextField*)wid->handle isBezeled] ? TRUE : FALSE;
		return ([(NSScrollView*)wid->handle borderType] != NSNoBorder) ? TRUE : FALSE;
	}
}


void Gui_Widget_Set_Border(GUIWIDGET *wid, REBOOL on)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return;
		if (wid->kind == W_GUI_WIDGET_FIELD) {
			NSTextField *field = (NSTextField*)wid->handle;
			[field setBezeled:(on ? YES : NO)];
			if (on) [field setBezelStyle:NSTextFieldSquareBezel];
			else    [field setBordered:NO];
			[field setDrawsBackground:YES];
		} else {
			[(NSScrollView*)wid->handle setBorderType:(on ? NSBezelBorder : NSNoBorder)];
		}
		[NSVIEW_OF(wid) setNeedsDisplay:YES];
		Display_Pending = TRUE;
	}
}


// scrollRowToVisible: already scrolls as little as it takes.
void Gui_Widget_Scroll_To_Item(GUIWIDGET *wid, REBINT n)
{
	@autoreleasepool {
		RebolGuiList *list = List_View_Of(wid);
		if (!list || n < 0 || n >= [list numberOfRows]) return;
		[list scrollRowToVisible:(NSInteger)n];
	}
}


REBDEC Gui_Widget_Get_Scroll(GUIWIDGET *wid)
{
	@autoreleasepool {
		NSScrollView *sv = nil;
		CGFloat span = 0, at = 0;

		if (!Scroll_Metrics_Of(wid, &sv, &span, &at)) return -1.0;
		if (span <= 0) return 0.0; // it all fits
		return (REBDEC)(at / span);
	}
}


REBOOL Gui_Widget_Set_Scroll(GUIWIDGET *wid, REBDEC where)
{
	@autoreleasepool {
		NSScrollView *sv = nil;
		CGFloat span = 0, at = 0;

		if (!Scroll_Metrics_Of(wid, &sv, &span, &at)) return FALSE;

		// The end goes through the TEXT, not the clip view: after the
		// string has just been replaced the document's height may not be
		// laid out yet, and scrollRangeToVisible: forces that first. A
		// computed fraction has no such guarantee to offer, so it uses
		// whatever the layout currently says.
		if (where >= 1.0 && wid->kind == W_GUI_WIDGET_TEXT_LIST) {
			RebolGuiList *list = List_View_Of(wid);
			if ([list numberOfRows] > 0)
				[list scrollRowToVisible:[list numberOfRows] - 1];
			return TRUE;
		}
		if (where >= 1.0) {
			NSTextView *text = Text_View_Of(wid);
			[text scrollRangeToVisible:
				NSMakeRange([[text string] length], 0)];
			return TRUE;
		}

		if (span <= 0) return TRUE; // nothing to scroll
		if (where < 0.0) where = 0.0;

		[[sv contentView] scrollToPoint:
			NSMakePoint(0, (CGFloat)(span * where))];
		[sv reflectScrolledClipView:[sv contentView]];
		return TRUE;
	}
}


REBOOL Gui_Widget_Set_Read_Only(GUIWIDGET *wid, REBOOL on)
{
	@autoreleasepool {
		if (!wid || !wid->handle) return FALSE;
		// `on` is already in wid->state; what matters here is that the
		// control is not disabled at the same time.
		Apply_Text_Editability(wid, Gui_Widget_Get_Enabled(wid));
		return TRUE;
	}
}