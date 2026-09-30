//
// Project: Rebol/GUI extension
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
// GTK 3 backend, for Linux and the other X11/Wayland systems.
//
// Implements exactly the same Gui_* API as gui-win.c and gui-mac.m, so
// nothing above it changes: commands, the event queue and the handle
// accessors are shared.
//
// Four decisions shape everything below.
//
//   * ONE container class. The window's client area, a panel, a tab page
//     and an image widget are all a RebolGuiBox: a GtkContainer which
//     places each child at exactly the box it was given - no layout, no
//     natural sizes - and draws what the kind needs (a background, a
//     frame and caption, pixels). GtkFixed would give a child its natural
//     size whenever that is larger than the box asked for.
//
//   * ONE place for the mouse and the keyboard. GTK hands every GdkEvent
//     to the handler installed with gdk_event_handler_set() before any
//     widget sees it. That hook reports `move`, `down`, `up`, the wheel,
//     `leave` and `keys?` for every window, whatever control is under the
//     pointer or holds the focus, and passes the event on unchanged - so
//     no control has to be subclassed to be observed. Everything a
//     control means (a click, an edit, a pick) comes from its signals.
//
//   * The host pumps. GTK's main loop never runs on its own: Gui_Pump()
//     iterates the default main context without blocking, from the device
//     poll inside WAIT, which is also when GTK lays out and paints.
//
//   * Coordinates are GTK's own: logical pixels, with the integer scale
//     factor applied by GDK. That is what gui.h calls a logical unit, so
//     nothing is converted here - as on macOS.
//
// X11 is preferred over Wayland when both are available (the GDK_BACKEND
// variable still decides): a Wayland client may not place its windows or
// ask where the pointer is outside them, and `offset`, screens and
// `track-mouse` all depend on that.
//

#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "gen-gui.h"
#include "gui.h"


/***********************************************************************
**  Type names.
**
**  GType names are process-wide, like Objective-C class names: a second
**  copy of these sources in the same process (a standalone .rebx next to
**  a host with the extension embedded) must register its types under
**  other names, or it gets the first copy's class structures. The nest
**  sets a prefix for the standalone build - see the note in gui-mac.m.
***********************************************************************/
#ifndef GUI_CLASS_PREFIX
#define GUI_CLASS_PREFIX RebolGuiEmbedded
#endif
#define GUI_STR2(a) #a
#define GUI_STR(a)  GUI_STR2(a)
#define GUI_TYPE_NAME(name) GUI_STR(GUI_CLASS_PREFIX) name

// Keys of the data this file hangs on GTK objects.
#define KEY_WIDGET  "rebol-gui-widget"   // GUIWIDGET* on a control and its inner parts
#define KEY_WINDOW  "rebol-gui-window"   // GUIWIN* on the GtkWindow
#define KEY_ITEM    "rebol-gui-item"     // menu item id
#define KEY_FIELD   "rebol-gui-field"    // a list-view column's field
#define KEY_INNER   "rebol-gui-inner"    // the text view / tree view in a scroller
#define KEY_STYLE   "rebol-gui-style"    // WSTYLE*
#define KEY_PROV    "rebol-gui-provider" // the provider already added to a context
#define KEY_DATE    "rebol-gui-date"     // DATEFIELD*
#define KEY_PARTNER "rebol-gui-partner"  // a radio's hidden group partner

// How far in from the left edge a framed panel's caption starts - the same
// number as in the other two backends.
#define PANEL_CAPTION_X 9

// The window-wide CSS class, so the compact metrics below reach only the
// windows this extension opens.
#define WINDOW_CLASS "rebol-gui"


//== per-window state =========================================================

typedef struct Gtk_Window_State {
	GtkWidget *window;   // the GtkWindow
	GtkWidget *vbox;     // menu bar above the client area
	GtkWidget *content;  // RebolGuiBox - the client area
	GtkWidget *menubar;  // NULL without a menu
	GtkAccelGroup *accel;
	GPtrArray *items;    // menu item id N is items[N-1]; NULLs for the gaps
	REBINT     want_w, want_h;     // the client size asked for, or last allocated
	REBINT     seen_w, seen_h;     // the last size a `resize` reported
	REBOOL     shown;              // shown at least once
	REBOOL     resizable;
	REBOOL     decorated;
	REBOOL     closing;
} GTKWIN;

#define GW(win)       ((GTKWIN*)((win)->handle))
#define GTKWINDOW(win) GTK_WINDOW(GW(win)->window)

// Every open window, for the things which are about all of them: a theme
// change, the pointer leaving.
static GList *Windows = NULL;


//== state of the pump ========================================================

static REBOOL Gtk_Ready    = FALSE;
static REBCNT Pump_Events  = 0;      // GdkEvents seen by the hook, for Gui_Pump()
static REBINT Quiet        = 0;      // >0 while the script writes to a control

// While a context menu is up, a pick is its answer rather than a
// `menu-select` - see Gui_Popup_Track().
static REBOOL Popup_Tracking = FALSE;
static REBCNT Popup_Pick     = 0;

// A press in progress: which source reports its drag and its `up` - the
// widget it went down on, or the window - and which buttons are held.
static REBHOB *Press_Source  = NULL;
static GUIWIN *Press_Window  = NULL;
static REBCNT  Press_Buttons = 0;
static REBCNT  Press_Kind    = 0;

// The previous press, for telling a double click - GDK's own
// GDK_2BUTTON_PRESS arrives only AFTER the second press, which has by then
// been reported as a plain `down`.
static guint32 Last_Press_Time   = 0;
static guint   Last_Press_Button = 0;
static gdouble Last_Press_X = 0, Last_Press_Y = 0;

// Sub-line wheel movement from smooth-scrolling devices, until it adds up
// to a line.
static gdouble Wheel_Rest = 0.0;

// A copy of the last button press in one of our windows. GTK wants the
// event which opened a context menu; popup-menu is called from Rebol, long
// after that event was dispatched, so it is kept here.
static GdkEvent *Last_Press = NULL;

static GtkCssProvider *Compact_Css = NULL;


//== helpers ==================================================================

// UTF-8 which is not necessarily terminated -> a fresh terminated copy.
static gchar *Dup_Text(const REBYTE *utf8, REBCNT len)
{
	if (!utf8 || len == 0) return g_strdup("");
	return g_strndup((const gchar*)utf8, (gsize)len);
}

// ... and a terminated UTF-8 C string -> a fresh Rebol string series.
static REBSER *To_Rebol(const gchar *utf8)
{
	if (!utf8) return NULL;
	if (!*utf8) return RL_MAKE_STRING(0, FALSE);
	return RL_DECODE_UTF_STRING((REBYTE*)utf8, (REBCNT)strlen(utf8), 8, FALSE, FALSE);
}

static REBINT Modifier_Bits(guint state)
{
	REBINT flags = 0;
	if (state & GDK_SHIFT_MASK)   flags |= GUI_FLAG_SHIFT;
	if (state & GDK_CONTROL_MASK) flags |= GUI_FLAG_CONTROL;
	if (state & GDK_MOD1_MASK)    flags |= GUI_FLAG_ALT;
	return flags;
}

// The modifiers held right now, for signals that come without an event.
static REBINT Current_Modifiers(void)
{
	GdkModifierType state = 0;
	if (gtk_get_current_event_state(&state)) return Modifier_Bits(state);
	return 0;
}

static void Rgba_Of(REBCNT c, GdkRGBA *out)
{
	out->red   = GUI_COLOR_R(c) / 255.0;
	out->green = GUI_COLOR_G(c) / 255.0;
	out->blue  = GUI_COLOR_B(c) / 255.0;
	out->alpha = 1.0;
}

static void Set_Source_Color(cairo_t *cr, REBCNT c)
{
	cairo_set_source_rgb(cr, GUI_COLOR_R(c) / 255.0,
	                         GUI_COLOR_G(c) / 255.0,
	                         GUI_COLOR_B(c) / 255.0);
}

/***********************************************************************
**  `&` marks a mnemonic in a label on Windows, `_` does in GTK.
**
**  So `&Save` becomes `_Save`, a literal `_` is doubled, and `&&` is a
**  literal `&`. Read back the other way, so what a script set is what it
**  gets - a button's GTK label is not the text the script gave it.
***********************************************************************/
static gchar *To_Mnemonic(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	for (p = text; *p; p++) {
		if (*p == '&') {
			if (p[1] == '&') { g_string_append_c(out, '&'); p++; }
			else g_string_append_c(out, '_');
		}
		else if (*p == '_') g_string_append(out, "__");
		else g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}

static gchar *From_Mnemonic(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	if (!text) text = "";
	for (p = text; *p; p++) {
		if (*p == '_') {
			if (p[1] == '_') { g_string_append_c(out, '_'); p++; }
			else g_string_append_c(out, '&');
		}
		else if (*p == '&') g_string_append(out, "&&");
		else g_string_append_c(out, *p);
	}
	return g_string_free(out, FALSE);
}

static GUIWIN *Window_Of_Gtk(GtkWidget *toplevel)
{
	if (!toplevel || !GTK_IS_WINDOW(toplevel)) return NULL;
	return (GUIWIN*)g_object_get_data(G_OBJECT(toplevel), KEY_WINDOW);
}

// The GUIWIN a GdkWindow belongs to - NULL for a menu, a combo box's
// popup, a tooltip, or anything else which is not one of ours.
static GUIWIN *Window_Of_Gdk(GdkWindow *gw)
{
	gpointer user = NULL;
	if (!gw) return NULL;
	gw = gdk_window_get_toplevel(gw);
	gdk_window_get_user_data(gw, &user);
	if (!user || !GTK_IS_WIDGET(user)) return NULL;
	return Window_Of_Gtk(GTK_WIDGET(user));
}

// The widget whose native control is (or contains) this GTK widget.
static GUIWIDGET *Widget_Of_Gtk(GtkWidget *w)
{
	while (w) {
		GUIWIDGET *wid = (GUIWIDGET*)g_object_get_data(G_OBJECT(w), KEY_WIDGET);
		if (wid) return wid;
		if (GTK_IS_WINDOW(w)) return NULL;
		w = gtk_widget_get_parent(w);
	}
	return NULL;
}

#define GTKW(wid)   ((GtkWidget*)((wid)->handle))

// The text view of an area, and the tree view of a text-list or a
// list-view: each sits in a GtkScrolledWindow, which is the widget.
static GtkWidget *Inner_Of(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;
	return (GtkWidget*)g_object_get_data(G_OBJECT(wid->handle), KEY_INNER);
}

#define IS_TABLE(wid) ((wid)->kind == W_GUI_WIDGET_TEXT_LIST \
                    || (wid)->kind == W_GUI_WIDGET_LIST_VIEW \
                    || (wid)->kind == W_GUI_WIDGET_TREE_VIEW)

// The entry of a drop-down, which is where its text and its focus are.
static GtkWidget *Combo_Entry(GUIWIDGET *wid)
{
	if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_DROP_DOWN) return NULL;
	return gtk_bin_get_child(GTK_BIN(wid->handle));
}


/***********************************************************************
**
**  RebolGuiBox - the one container.
**
**  Children sit at the box they were given, in the box's own coordinates,
**  and are allocated exactly that - GTK's natural sizes are only asked
**  for, as GTK requires before an allocation, and then ignored. What the
**  box draws depends on its role:
**
**    content   the window's background colour, when it has one; and the
**              thin outline of a borderless window with `border?`
**    panel     a background colour, and the frame and caption
**    image     the pixels of its image!, stretched to the box
**
**  Each has a GdkWindow of its own: that clips the children to the box,
**  and it is what mouse events are delivered to.
**
***********************************************************************/

enum {
	ROLE_CONTENT = 1,
	ROLE_PANEL,
	ROLE_IMAGE
};

typedef struct {
	GtkWidget *widget;
	REBINT x, y, w, h;
} BOXCHILD;

typedef struct {
	GtkContainer parent;
	GList     *children;  // BOXCHILD*, in creation order - later is on top
	REBCNT     role;
	GUIWIN    *win;       // content: the window
	GUIWIDGET *wid;       // panel / image: the widget
	REBINT     nat_w, nat_h;  // content: what the window is sized to
} RebolGuiBox;

typedef struct {
	GtkContainerClass parent_class;
} RebolGuiBoxClass;

static GType Box_Type(void);
#define REBOL_GUI_BOX(o) ((RebolGuiBox*)(o))
#define IS_BOX(o)        G_TYPE_CHECK_INSTANCE_TYPE((o), Box_Type())

static GtkContainerClass *Box_Parent_Class = NULL;

static BOXCHILD *Box_Find(RebolGuiBox *box, GtkWidget *child)
{
	GList *l;
	for (l = box->children; l; l = l->next)
		if (((BOXCHILD*)l->data)->widget == child) return (BOXCHILD*)l->data;
	return NULL;
}

static void Box_Put(GtkWidget *box, GtkWidget *child, REBINT x, REBINT y, REBINT w, REBINT h)
{
	RebolGuiBox *b = REBOL_GUI_BOX(box);
	BOXCHILD *c = g_new0(BOXCHILD, 1);
	c->widget = child;
	c->x = x; c->y = y; c->w = w; c->h = h;
	b->children = g_list_append(b->children, c);
	gtk_widget_set_parent(child, box);
}

static void Box_Move(GtkWidget *box, GtkWidget *child, REBINT x, REBINT y, REBINT w, REBINT h)
{
	BOXCHILD *c = Box_Find(REBOL_GUI_BOX(box), child);
	if (!c) return;
	c->x = x; c->y = y; c->w = w; c->h = h;
	gtk_widget_queue_resize(child);
	gtk_widget_queue_allocate(box);
	gtk_widget_queue_draw(box);
}

static void Box_Add(GtkContainer *container, GtkWidget *child)
{
	Box_Put(GTK_WIDGET(container), child, 0, 0, 1, 1);
}

static void Box_Remove(GtkContainer *container, GtkWidget *child)
{
	RebolGuiBox *b = REBOL_GUI_BOX(container);
	BOXCHILD *c = Box_Find(b, child);
	gboolean visible;
	if (!c) return;
	visible = gtk_widget_get_visible(child);
	gtk_widget_unparent(child);
	b->children = g_list_remove(b->children, c);
	g_free(c);
	if (visible && gtk_widget_get_visible(GTK_WIDGET(container)))
		gtk_widget_queue_draw(GTK_WIDGET(container));
}

static void Box_Forall(GtkContainer *container, gboolean internals,
                       GtkCallback callback, gpointer data)
{
	GList *l = REBOL_GUI_BOX(container)->children;
	while (l) {
		BOXCHILD *c = (BOXCHILD*)l->data;
		l = l->next;   // the callback may remove the child
		(*callback)(c->widget, data);
	}
}

static GType Box_Child_Type(GtkContainer *container)
{
	return GTK_TYPE_WIDGET;
}

static void Box_Realize(GtkWidget *widget)
{
	GtkAllocation a;
	GdkWindowAttr attr;
	GdkWindow *win;

	gtk_widget_set_realized(widget, TRUE);
	gtk_widget_get_allocation(widget, &a);

	memset(&attr, 0, sizeof(attr));
	attr.window_type = GDK_WINDOW_CHILD;
	attr.x = a.x;
	attr.y = a.y;
	attr.width  = a.width;
	attr.height = a.height;
	attr.wclass = GDK_INPUT_OUTPUT;
	attr.visual = gtk_widget_get_visual(widget);
	attr.event_mask = gtk_widget_get_events(widget)
		| GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK
		| GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
		| GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK
		| GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK;

	win = gdk_window_new(gtk_widget_get_parent_window(widget), &attr,
	                     GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
	gtk_widget_set_window(widget, win);
	gtk_widget_register_window(widget, win);
}

// The content box asks for what the window should be; the rest ask for
// nothing, since whatever holds them gives them their box.
static void Box_Preferred_Width(GtkWidget *widget, gint *min, gint *nat)
{
	RebolGuiBox *b = REBOL_GUI_BOX(widget);
	*min = 1;
	*nat = (b->role == ROLE_CONTENT && b->nat_w > 0) ? b->nat_w : 1;
	if (b->role == ROLE_CONTENT && b->win && b->win->handle && !GW(b->win)->resizable)
		*min = *nat;
}

static void Box_Preferred_Height(GtkWidget *widget, gint *min, gint *nat)
{
	RebolGuiBox *b = REBOL_GUI_BOX(widget);
	*min = 1;
	*nat = (b->role == ROLE_CONTENT && b->nat_h > 0) ? b->nat_h : 1;
	if (b->role == ROLE_CONTENT && b->win && b->win->handle && !GW(b->win)->resizable)
		*min = *nat;
}

/***********************************************************************
**  Each child gets exactly its box. GTK insists on being asked for a
**  preferred size before an allocation, so it is - and then the box is
**  used as it is, whatever the answer. A box smaller than a control's
**  minimum clips it, as on the other platforms.
***********************************************************************/
static void Box_Size_Allocate(GtkWidget *widget, GtkAllocation *allocation)
{
	RebolGuiBox *b = REBOL_GUI_BOX(widget);
	GList *l;

	gtk_widget_set_allocation(widget, allocation);
	if (gtk_widget_get_realized(widget))
		gdk_window_move_resize(gtk_widget_get_window(widget),
		                       allocation->x, allocation->y,
		                       allocation->width, allocation->height);

	for (l = b->children; l; l = l->next) {
		BOXCHILD *c = (BOXCHILD*)l->data;
		GtkRequisition req;
		GtkAllocation ca;
		// A control is created at a 1 unit placeholder when its size is
		// still to be measured. Laid out at that, GTK warns that its frame
		// does not fit - so it is left out until it has a real box. A line
		// is the one kind that can be that thin on purpose.
		gboolean tiny = (c->w <= 1 || c->h <= 1) && !GTK_IS_SEPARATOR(c->widget);
		if (gtk_widget_get_child_visible(c->widget) == tiny)
			gtk_widget_set_child_visible(c->widget, !tiny);
		if (tiny || !gtk_widget_get_visible(c->widget)) continue;
		gtk_widget_get_preferred_size(c->widget, &req, NULL);
		ca.x = c->x;
		ca.y = c->y;
		ca.width  = c->w > 0 ? c->w : 1;
		ca.height = c->h > 0 ? c->h : 1;
		gtk_widget_size_allocate(c->widget, &ca);
	}
}

static void Draw_Panel_Frame(GtkWidget *widget, cairo_t *cr, GUIWIDGET *wid);
static void Draw_Image(GtkWidget *widget, cairo_t *cr, GUIWIDGET *wid);

// A fill of the box itself. Not cairo_paint(): the clip GTK hands a
// widget with a window of its own can reach past it, into its parent.
static void Fill_Box(cairo_t *cr, REBCNT color, gint w, gint h)
{
	Set_Source_Color(cr, color);
	cairo_rectangle(cr, 0, 0, w, h);
	cairo_fill(cr);
}

static gboolean Box_Draw(GtkWidget *widget, cairo_t *cr)
{
	RebolGuiBox *b = REBOL_GUI_BOX(widget);
	gint w = gtk_widget_get_allocated_width(widget);
	gint h = gtk_widget_get_allocated_height(widget);

	switch (b->role) {
	case ROLE_CONTENT:
		if (b->win && GUI_COLOR_HAS(b->win->background))
			Fill_Box(cr, b->win->background, w, h);
		break;
	case ROLE_PANEL:
		if (b->wid && GUI_COLOR_HAS(b->wid->background))
			Fill_Box(cr, b->wid->background, w, h);
		if (b->wid) Draw_Panel_Frame(widget, cr, b->wid);
		break;
	case ROLE_IMAGE:
		if (b->wid) {
			if (GUI_COLOR_HAS(b->wid->background))
				Fill_Box(cr, b->wid->background, w, h);
			Draw_Image(widget, cr, b->wid);
		}
		break;
	}

	GTK_WIDGET_CLASS(Box_Parent_Class)->draw(widget, cr);

	// A window with no title bar and `border?` gets a thin outline - drawn
	// last, so nothing in the window covers it.
	if (b->role == ROLE_CONTENT && b->win && b->win->handle
	    && !GW(b->win)->decorated && (b->win->flags & GUIW_BORDER)) {
		cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.8);
		cairo_set_line_width(cr, 1.0);
		cairo_rectangle(cr, 0.5, 0.5, w - 1.0, h - 1.0);
		cairo_stroke(cr);
	}
	return FALSE;
}

static void Box_Finalize(GObject *object)
{
	RebolGuiBox *b = REBOL_GUI_BOX(object);
	g_list_free_full(b->children, g_free);
	b->children = NULL;
	G_OBJECT_CLASS(Box_Parent_Class)->finalize(object);
}

static void Box_Class_Init(gpointer klass, gpointer data)
{
	GObjectClass      *oc = G_OBJECT_CLASS(klass);
	GtkWidgetClass    *wc = GTK_WIDGET_CLASS(klass);
	GtkContainerClass *cc = GTK_CONTAINER_CLASS(klass);

	Box_Parent_Class = GTK_CONTAINER_CLASS(g_type_class_peek_parent(klass));

	oc->finalize = Box_Finalize;
	wc->realize  = Box_Realize;
	wc->size_allocate = Box_Size_Allocate;
	wc->draw     = Box_Draw;
	wc->get_preferred_width  = Box_Preferred_Width;
	wc->get_preferred_height = Box_Preferred_Height;
	cc->add    = Box_Add;
	cc->remove = Box_Remove;
	cc->forall = Box_Forall;
	cc->child_type = Box_Child_Type;
	gtk_widget_class_set_css_name(wc, "rebolguibox");
}

static void Box_Init(GTypeInstance *instance, gpointer klass)
{
	gtk_widget_set_has_window(GTK_WIDGET(instance), TRUE);
	gtk_widget_set_can_focus(GTK_WIDGET(instance), FALSE);
}

static GType Box_Type(void)
{
	static GType type = 0;
	if (!type) {
		GTypeInfo info;
		memset(&info, 0, sizeof(info));
		info.class_size    = sizeof(RebolGuiBoxClass);
		info.class_init    = Box_Class_Init;
		info.instance_size = sizeof(RebolGuiBox);
		info.instance_init = Box_Init;
		type = g_type_register_static(GTK_TYPE_CONTAINER,
		                              GUI_TYPE_NAME("Box"), &info, 0);
	}
	return type;
}

static GtkWidget *New_Box(REBCNT role)
{
	GtkWidget *box = GTK_WIDGET(g_object_new(Box_Type(), NULL));
	REBOL_GUI_BOX(box)->role = role;
	return box;
}


/***********************************************************************
**  The panel's frame: drawn INSIDE its own box and moving nothing, with
**  the caption in a gap in the top line - the same rules as the other
**  two backends. No fill: the background, when there is one, is painted
**  before this, and a container otherwise shows what is behind it.
***********************************************************************/
static void Draw_Panel_Frame(GtkWidget *widget, cairo_t *cr, GUIWIDGET *wid)
{
	GtkStyleContext *ctx;
	GdkRGBA fg;
	gint w, h;
	gdouble inset = 0.0, gap1 = 0.0, gap2 = 0.0;
	PangoLayout *layout = NULL;
	gchar *caption;

	if (!(wid->state & GUI_PANEL_BORDER)) return;

	w = gtk_widget_get_allocated_width(widget);
	h = gtk_widget_get_allocated_height(widget);
	ctx = gtk_widget_get_style_context(widget);
	gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);

	caption = (gchar*)g_object_get_data(G_OBJECT(widget), "rebol-gui-caption");
	if (caption && *caption) {
		PangoRectangle ink, logical;
		layout = gtk_widget_create_pango_layout(widget, caption);
		pango_layout_get_pixel_extents(layout, &ink, &logical);
		inset = floor(logical.height / 2.0);
		gap1 = PANEL_CAPTION_X - 2.0;
		gap2 = gap1 + logical.width + 4.0;
		if (gap2 > w - 1.0) gap2 = w - 1.0;
	}

	if (w > 1 && h - inset > 1) {
		// The frame is a quiet version of the text colour, so it follows a
		// dark theme with nothing to decide here.
		cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha * 0.3);
		cairo_set_line_width(cr, 1.0);
		if (layout) {
			cairo_move_to(cr, 0.5, inset + 0.5);
			cairo_line_to(cr, gap1, inset + 0.5);
			cairo_move_to(cr, gap2, inset + 0.5);
			cairo_line_to(cr, w - 0.5, inset + 0.5);
		} else {
			cairo_move_to(cr, 0.5, inset + 0.5);
			cairo_line_to(cr, w - 0.5, inset + 0.5);
		}
		cairo_move_to(cr, w - 0.5, inset + 0.5);
		cairo_line_to(cr, w - 0.5, h - 0.5);
		cairo_line_to(cr, 0.5, h - 0.5);
		cairo_line_to(cr, 0.5, inset + 0.5);
		cairo_stroke(cr);
	}

	if (layout) {
		if (GUI_COLOR_HAS(wid->color)) Set_Source_Color(cr, wid->color);
		else gdk_cairo_set_source_rgba(cr, &fg);
		cairo_move_to(cr, PANEL_CAPTION_X, 0);
		pango_cairo_show_layout(cr, layout);
		g_object_unref(layout);
	}
}

/***********************************************************************
**  An image widget's pixels, read from the image! at every paint.
**
**  An image! is BGRA in memory, which on a little-endian machine is
**  exactly cairo's RGB24: 32 bits per pixel, the high byte unused. The
**  surface wraps the series in place, and does not outlive the paint -
**  the series can move as soon as Rebol runs again.
***********************************************************************/
static void Draw_Image(GtkWidget *widget, cairo_t *cr, GUIWIDGET *wid)
{
	REBYTE *bits = NULL;
	REBINT  iw = 0, ih = 0;
	gint w = gtk_widget_get_allocated_width(widget);
	gint h = gtk_widget_get_allocated_height(widget);
	cairo_surface_t *surface;
	cairo_pattern_t *pattern;

	// Nothing to show yet: draw NOTHING, and in particular do not fill -
	// see the note on the image view in gui-mac.m.
	if (!Gui_Widget_Pixels(wid, &bits, &iw, &ih)) return;
	if (iw <= 0 || ih <= 0 || w <= 0 || h <= 0) return;

	surface = cairo_image_surface_create_for_data(bits, CAIRO_FORMAT_RGB24,
	                                              iw, ih, iw * 4);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cairo_save(cr);
	cairo_scale(cr, (double)w / iw, (double)h / ih);
	cairo_set_source_surface(cr, surface, 0, 0);
	pattern = cairo_get_source(cr);
	cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
	cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
	cairo_rectangle(cr, 0, 0, iw, ih);
	cairo_fill(cr);
	cairo_restore(cr);
	cairo_surface_destroy(surface);
}


//== where things are =========================================================

// The client area of a window, as a GdkWindow - what every mouse position
// is measured from.
static GdkWindow *Content_Window(GUIWIN *win)
{
	if (!win || !win->handle || !GW(win)->content) return NULL;
	return gtk_widget_get_window(GW(win)->content);
}

// A point on the screen (an event's root coordinates) in the window's
// client coordinates.
static void Client_Point(GUIWIN *win, gdouble rx, gdouble ry, gdouble *cx, gdouble *cy)
{
	GdkWindow *gw = Content_Window(win);
	gint ox = 0, oy = 0;
	if (gw) gdk_window_get_origin(gw, &ox, &oy);
	*cx = rx - ox;
	*cy = ry - oy;
}

// Where a widget's top-left corner is in its window's client area, from
// the boxes it is nested in. A tab page is placed by its notebook, which
// is asked instead.
static REBOOL Widget_Origin(GUIWIDGET *wid, REBINT *x, REBINT *y)
{
	REBINT ax = 0, ay = 0;

	while (wid) {
		GtkWidget *w = GTKW(wid);
		GtkWidget *parent;
		if (!w) return FALSE;
		parent = gtk_widget_get_parent(w);
		if (parent && IS_BOX(parent)) {
			BOXCHILD *c = Box_Find(REBOL_GUI_BOX(parent), w);
			if (!c) return FALSE;
			ax += c->x;
			ay += c->y;
		}
		else if (parent && GTK_IS_NOTEBOOK(parent)) {
			GtkAllocation pa, na;
			gtk_widget_get_allocation(w, &pa);
			gtk_widget_get_allocation(parent, &na);
			// Allocations of widgets without a window of their own are in
			// the coordinates of the nearest one which has - here the box
			// holding the notebook, whose own offset is added next.
			ax += pa.x - na.x;
			ay += pa.y - na.y;
		}
		else return FALSE;
		wid = (GUIWIDGET*)wid->parent;
	}
	*x = ax;
	*y = ay;
	return TRUE;
}

static REBOOL Widget_Shown(GUIWIDGET *wid)
{
	GtkWidget *w = GTKW(wid);
	return w && gtk_widget_get_visible(w) && gtk_widget_get_child_visible(w)
	    && (gtk_widget_get_mapped(w) || !gtk_widget_get_realized(w));
}

static REBOOL Widget_Contains(GUIWIDGET *wid, gdouble cx, gdouble cy)
{
	REBINT x, y;
	GtkWidget *w = GTKW(wid);
	GtkWidget *parent;
	BOXCHILD *c;
	REBINT bw, bh;

	if (!Widget_Origin(wid, &x, &y)) return FALSE;
	parent = gtk_widget_get_parent(w);
	if (parent && IS_BOX(parent) && (c = Box_Find(REBOL_GUI_BOX(parent), w))) {
		bw = c->w; bh = c->h;
	} else {
		bw = gtk_widget_get_allocated_width(w);
		bh = gtk_widget_get_allocated_height(w);
	}
	return cx >= x && cy >= y && cx < x + bw && cy < y + bh;
}

/***********************************************************************
**  The widget a `move` at this point is about: the DEEPEST one under
**  it, or NULL for the window's own background - one level at a time,
**  through the window's own list, as gui-mac.m does it.
**
**  A label and a line are skipped, as if the pointer went through them
**  (a Win32 static answers HTTRANSPARENT), and so is a disabled control,
**  whose mouse input Win32 hands to the parent. A tab page whose tab is
**  not the one shown is not mapped, and is skipped too.
***********************************************************************/
static GUIWIDGET *Widget_Under_Point(GUIWIN *win, gdouble cx, gdouble cy, REBOOL direct)
{
	GUIWIDGET *found = NULL;
	GUIWIDGET *wid;
	REBOOL deeper = TRUE;

	if (!win) return NULL;
	while (deeper) {
		deeper = FALSE;
		// The list is in reverse creation order, so the first match at a
		// level is the topmost one there.
		for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next) {
			if ((GUIWIDGET*)wid->parent != found || !wid->handle) continue;
			if (!Widget_Shown(wid)) continue;
			if (!direct) {
				if (wid->kind == W_GUI_WIDGET_TEXT || wid->kind == W_GUI_WIDGET_LINE) continue;
				if (!gtk_widget_is_sensitive(GTKW(wid))) continue;
			}
			if (Widget_Contains(wid, cx, cy)) {
				found  = wid;
				deeper = !direct;
				break;
			}
		}
	}
	return found;
}

// The kinds whose presses are theirs to report: what Rebol draws on, and
// the controls one presses. The rest take the mouse for themselves (a
// field selects text, a list picks a row) and report what it meant; a
// panel only holds things.
static REBOOL Reports_Press(REBCNT kind)
{
	switch (kind) {
	case W_GUI_WIDGET_IMAGE:
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_TOGGLE:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
	case W_GUI_WIDGET_SLIDER:
		return TRUE;
	}
	return FALSE;
}


//== the event hook ===========================================================

static REBCNT Named_Key_Of(guint keyval)
{
	if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)
		return EVK_F1 + (REBCNT)(keyval - GDK_KEY_F1);
	switch (keyval) {
	case GDK_KEY_Page_Up:   case GDK_KEY_KP_Page_Up:   return EVK_PAGE_UP;
	case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return EVK_PAGE_DOWN;
	case GDK_KEY_End:       case GDK_KEY_KP_End:       return EVK_END;
	case GDK_KEY_Home:      case GDK_KEY_KP_Home:      return EVK_HOME;
	case GDK_KEY_Left:      case GDK_KEY_KP_Left:      return EVK_LEFT;
	case GDK_KEY_Up:        case GDK_KEY_KP_Up:        return EVK_UP;
	case GDK_KEY_Right:     case GDK_KEY_KP_Right:     return EVK_RIGHT;
	case GDK_KEY_Down:      case GDK_KEY_KP_Down:      return EVK_DOWN;
	case GDK_KEY_Insert:    case GDK_KEY_KP_Insert:    return EVK_INSERT;
	case GDK_KEY_Delete:    case GDK_KEY_KP_Delete:    return EVK_DELETE;
	case GDK_KEY_Escape:     return EVK_ESCAPE;
	case GDK_KEY_Shift_L:   case GDK_KEY_Shift_R:      return EVK_SHIFT;
	case GDK_KEY_Control_L: case GDK_KEY_Control_R:    return EVK_CONTROL;
	case GDK_KEY_Alt_L:     case GDK_KEY_Alt_R:
	case GDK_KEY_Meta_L:    case GDK_KEY_Meta_R:
	case GDK_KEY_ISO_Level3_Shift:                     return EVK_ALT;
	case GDK_KEY_Pause:      return EVK_PAUSE;
	case GDK_KEY_Caps_Lock:  return EVK_CAPITAL;
	case GDK_KEY_ISO_Left_Tab: return EVK_BACKTAB;
	case GDK_KEY_BackSpace:  return EVK_BACKSPACE;
	case GDK_KEY_KP_Begin: case GDK_KEY_Begin: return EVK_BEGIN;
	}
	return 0;
}

/***********************************************************************
**  `keys?`: every key in a window which asked for them, reported and
**  passed on. The source is the widget holding the focus, or the window.
**
**  The character is the key's own with Shift applied and Control not -
**  GDK's keyval already is exactly that - so Ctrl+A is #"a" with
**  `control` in the flags, as on the other platforms.
***********************************************************************/
static void Report_Key(GUIWIN *win, GdkEventKey *key)
{
	GtkWidget *focus;
	GUIWIDGET *wid = NULL;
	REBHOB    *source;
	REBINT     mods = Modifier_Bits(key->state);
	REBCNT     named;
	REBOOL     up = (key->type == GDK_KEY_RELEASE);
	gunichar   code;

	focus = gtk_window_get_focus(GTKWINDOW(win));
	if (focus) wid = Widget_Of_Gtk(focus);
	source = (wid && wid->hob) ? wid->hob : win->hob;

	named = Named_Key_Of(key->keyval);
	if (named) {
		Gui_Queue_Key(source, up ? EVT_NAMED_KEY_UP : EVT_NAMED_KEY, named, mods);
		return;
	}
	switch (key->keyval) {
	case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
		code = '\r'; break;
	case GDK_KEY_Tab: case GDK_KEY_KP_Tab:
		code = '\t'; break;
	default:
		code = gdk_keyval_to_unicode(key->keyval);
	}
	if (code == 0) return;   // a dead key, or one with no character
	Gui_Queue_Key(source, up ? EVT_KEY_UP : EVT_KEY, (REBU32)code, mods);
}

static REBCNT Button_Down_Type(guint button)
{
	switch (button) {
	case 1: return EVT_DOWN;
	case 2: return EVT_AUX_DOWN;
	case 3: return EVT_ALT_DOWN;
	}
	return 0;
}

static REBCNT Button_Up_Type(guint button)
{
	switch (button) {
	case 1: return EVT_UP;
	case 2: return EVT_AUX_UP;
	case 3: return EVT_ALT_UP;
	}
	return 0;
}

// GDK's own rule for a double click, applied at the second press rather
// than after it.
static REBOOL Is_Double_Click(GdkEventButton *b)
{
	GtkSettings *settings = gtk_settings_get_default();
	gint time_ms = 400, distance = 5;
	REBOOL dbl;

	if (settings) g_object_get(settings, "gtk-double-click-time", &time_ms,
	                           "gtk-double-click-distance", &distance, NULL);
	dbl = Last_Press_Button == b->button
	   && (b->time - Last_Press_Time) <= (guint32)time_ms
	   && fabs(b->x_root - Last_Press_X) <= distance
	   && fabs(b->y_root - Last_Press_Y) <= distance;

	// A third click starts again, as on the other platforms.
	Last_Press_Time   = dbl ? 0 : b->time;
	Last_Press_Button = b->button;
	Last_Press_X = b->x_root;
	Last_Press_Y = b->y_root;
	return dbl;
}

static void Bring_Modal_Forward(void)
{
	GUIWIN *top = Gui_Modal_Top();
	if (top && top->handle) gtk_window_present(GTKWINDOW(top));
}

// Whether a pointer event is about the client area - not the menu bar
// above it, nor a date field's calendar, which share its toplevel.
static REBOOL In_Client(GUIWIN *win, GdkEvent *ev)
{
	GtkWidget *w = gtk_get_event_widget(ev);
	GtkWidget *content = GW(win)->content;
	return w && (w == content || gtk_widget_is_ancestor(w, content));
}

/***********************************************************************
**  Every GdkEvent of the process passes through here, before GTK
**  dispatches it - see the note at the top of the file.
***********************************************************************/
static void Event_Hook(GdkEvent *ev, gpointer data)
{
	GUIWIN   *win;
	gdouble   cx, cy;
	REBCNT    type;
	REBHOB   *source;
	GUIWIDGET *wid;
	REBHOB   *after_source = NULL;   // an `up` reported after GTK has seen it
	REBCNT    after_type = 0;
	REBINT    after_x = 0, after_y = 0, after_mods = 0;

	Pump_Events++;
	win = Window_Of_Gdk(ev->any.window);

	if (win && win->hob && GW(win) && !GW(win)->closing) {
		switch (ev->type) {

		case GDK_BUTTON_PRESS: {
			GdkEventButton *b = &ev->button;
			REBINT extra;
			if (Gui_Window_Blocked(win)) { Bring_Modal_Forward(); return; }
			type = Button_Down_Type(b->button);
			if (!type || !In_Client(win, ev)) break;
			extra = Is_Double_Click(b) ? GUI_FLAG_DOUBLE : 0;
			Client_Point(win, b->x_root, b->y_root, &cx, &cy);
			// A press on the window's own background is the window's; one
			// on a panel is nobody's, as the README promises.
			wid = Widget_Under_Point(win, cx, cy, FALSE);
			if (!wid) source = win->hob;
			else if (Reports_Press(wid->kind)) source = wid->hob;
			else source = NULL;   // the control's own business
			if (Press_Buttons == 0) {
				Press_Source = source;
				Press_Window = win;
				Press_Kind   = wid ? wid->kind : 0;
			}
			Press_Buttons |= 1u << b->button;
			if (Last_Press) gdk_event_free(Last_Press);
			Last_Press = gdk_event_copy(ev);
			if (source)
				Gui_Queue_Event(source, type, (REBINT)floor(cx), (REBINT)floor(cy),
				                Modifier_Bits(b->state) | extra);
			break; }

		case GDK_2BUTTON_PRESS:
		case GDK_3BUTTON_PRESS:
			if (Gui_Window_Blocked(win)) return;
			break;

		case GDK_BUTTON_RELEASE: {
			GdkEventButton *b = &ev->button;
			if (Gui_Window_Blocked(win) && !(Press_Buttons & (1u << b->button))) return;
			type = Button_Up_Type(b->button);
			if (!type || !(Press_Buttons & (1u << b->button))) {
				Press_Buttons &= ~(1u << b->button);
				break;
			}
			Press_Buttons &= ~(1u << b->button);
			if (Press_Window == win && Press_Source) {
				Client_Point(win, b->x_root, b->y_root, &cx, &cy);
				// A slider settles its value on release; `up` is the end of
				// the drag, so it follows the last `change`.
				if (Press_Kind == W_GUI_WIDGET_SLIDER && Press_Source != win->hob) {
					after_source = Press_Source;
					after_type = type;
					after_x = (REBINT)floor(cx);
					after_y = (REBINT)floor(cy);
					after_mods = Modifier_Bits(b->state);
				} else {
					Gui_Queue_Event(Press_Source, type, (REBINT)floor(cx), (REBINT)floor(cy),
					                Modifier_Bits(b->state));
				}
			}
			if (Press_Buttons == 0) {
				Press_Source = NULL;
				Press_Window = NULL;
				Press_Kind   = 0;
			}
			break; }

		case GDK_MOTION_NOTIFY: {
			GdkEventMotion *m = &ev->motion;
			if (Gui_Window_Blocked(win)) return;
			Client_Point(win, m->x_root, m->y_root, &cx, &cy);
			if (Press_Buttons) {
				if (Press_Window != win) break;
				// The drag belongs to whatever the press went down on,
				// wherever the pointer is now.
				if (Press_Window == win && Press_Source)
					Gui_Queue_Event(Press_Source, EVT_MOVE, (REBINT)floor(cx),
					                (REBINT)floor(cy), Modifier_Bits(m->state));
				break;
			}
			if (!In_Client(win, ev)) break;
			wid = Widget_Under_Point(win, cx, cy, FALSE);
			source = (wid && wid->hob) ? wid->hob : win->hob;
			Gui_Queue_Event(source, EVT_MOVE, (REBINT)floor(cx), (REBINT)floor(cy),
			                Modifier_Bits(m->state));
			break; }

		case GDK_SCROLL: {
			GdkEventScroll *s = &ev->scroll;
			REBINT lines = 0;
			if (Gui_Window_Blocked(win)) return;
			switch (s->direction) {
			case GDK_SCROLL_UP:   lines =  1; break;
			case GDK_SCROLL_DOWN: lines = -1; break;
			case GDK_SCROLL_SMOOTH:
				// A wheel notch is 1.0; a touchpad reports fractions, which
				// are added up until they make a line.
				Wheel_Rest -= s->delta_y;
				if (Wheel_Rest >= 1.0 || Wheel_Rest <= -1.0) {
					lines = (REBINT)Wheel_Rest;
					Wheel_Rest -= lines;
				}
				break;
			default: break;
			}
			if (lines == 0 || !In_Client(win, ev)) break;
			Client_Point(win, s->x_root, s->y_root, &cx, &cy);
			// A control which scrolls takes the wheel for itself.
			wid = Widget_Under_Point(win, cx, cy, FALSE);
			if (wid && (wid->kind == W_GUI_WIDGET_AREA
			         || wid->kind == W_GUI_WIDGET_LIST_VIEW
			         || (wid->kind == W_GUI_WIDGET_TEXT_LIST && !(wid->state & GUI_LIST_FIXED))))
				break;
			Gui_Queue_Event(win->hob, EVT_SCROLL_LINE, (REBINT)floor(cx), (REBINT)floor(cy), lines);
			break; }

		case GDK_LEAVE_NOTIFY: {
			GdkEventCrossing *c = &ev->crossing;
			// Out of the window altogether: not into one of its own parts,
			// not a grab starting, and not during a press - the pressed
			// control keeps the pointer until the button is up.
			if (c->window == gtk_widget_get_window(GW(win)->window)
			    && c->detail != GDK_NOTIFY_INFERIOR
			    && c->mode == GDK_CROSSING_NORMAL
			    && Press_Buttons == 0)
				Gui_Pointer_Left();
			break; }

		case GDK_KEY_PRESS:
		case GDK_KEY_RELEASE:
			if (Gui_Window_Blocked(win)) return;
			if (win->flags & GUIW_KEYS) Report_Key(win, &ev->key);
			break;

		default:
			break;
		}
	}
	else if (ev->type == GDK_BUTTON_RELEASE && Press_Buttons) {
		// Released over something which is not ours - a menu - so the
		// press is over, even though there is nobody to tell.
		Press_Buttons &= ~(1u << ev->button.button);
		if (Press_Buttons == 0) { Press_Source = NULL; Press_Window = NULL; Press_Kind = 0; }
	}

	gtk_main_do_event(ev);

	if (after_source) Gui_Queue_Event(after_source, after_type, after_x, after_y, after_mods);
}


//== platform =================================================================

/***********************************************************************
**  Compact metrics.
**
**  Adwaita sizes its controls for touch as much as for a mouse: a push
**  button is at least 34 pixels tall, an entry as much. A layout written
**  for the other two platforms - a 24 pixel button - would clip them.
**  Inside this extension's windows, minimum heights go and the padding
**  is trimmed, so a control's natural size is its text and a small
**  margin, about what Windows and macOS measure.
***********************************************************************/
static const char Compact_Rules[] =
	"window." WINDOW_CLASS " button,"
	"window." WINDOW_CLASS " entry,"
	"window." WINDOW_CLASS " spinbutton,"
	"window." WINDOW_CLASS " combobox button,"
	"window." WINDOW_CLASS " notebook > header > tabs > tab {"
	"  min-height: 0; min-width: 0; }"
	"window." WINDOW_CLASS " button { padding: 2px 8px; }"
	"window." WINDOW_CLASS " combobox button { padding: 2px 6px; }"
	"window." WINDOW_CLASS " entry { padding-top: 2px; padding-bottom: 2px; }"
	"window." WINDOW_CLASS " checkbutton, window." WINDOW_CLASS " radiobutton {"
	"  min-height: 0; padding: 0; }"
	"window." WINDOW_CLASS " notebook > header > tabs > tab { padding: 3px 10px; }"
	"window." WINDOW_CLASS " scale { min-height: 0; min-width: 0; }"
	"window." WINDOW_CLASS " scrollbar.vertical slider { min-height: 8px; }"
	"window." WINDOW_CLASS " scrollbar.horizontal slider { min-width: 8px; }"
	"window." WINDOW_CLASS " scale.horizontal { padding: 0 8px; }"
	"window." WINDOW_CLASS " scale.vertical { padding: 8px 0; }"
	"window." WINDOW_CLASS " rebolguibox { background: none; }";

static void Theme_Notify(GObject *settings, GParamSpec *spec, gpointer data);

void Gui_Init_Platform(void)
{
	GtkSettings *settings;

	if (Gtk_Ready) return;

	// X11 first, where there is one - see the note at the top.
	gdk_set_allowed_backends("x11,*");
	if (!gtk_init_check(NULL, NULL)) return;   // no display: every open fails

	gdk_event_handler_set(Event_Hook, NULL, NULL);

	Compact_Css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(Compact_Css, Compact_Rules, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(Compact_Css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	settings = gtk_settings_get_default();
	if (settings) {
		g_signal_connect(settings, "notify::gtk-theme-name", G_CALLBACK(Theme_Notify), NULL);
		g_signal_connect(settings, "notify::gtk-application-prefer-dark-theme",
		                 G_CALLBACK(Theme_Notify), NULL);
	}
	Gtk_Ready = TRUE;
}

void Gui_Quit_Platform(void)
{
	// Windows still open belong to a process which is going away; they go
	// with its X connection. Nothing to unregister.
}


//-- window signals -----------------------------------------------------------

// Reported only; the window stays open until Rebol calls `close-window`.
static gboolean On_Delete(GtkWidget *window, GdkEvent *ev, gpointer data)
{
	GUIWIN *win = Window_Of_Gtk(window);
	if (win && win->hob && !Gui_Window_Blocked(win))
		Gui_Queue_Event(win->hob, EVT_CLOSE, 0, 0, 0);
	return TRUE;
}

// The client area's size: what `size` reports from now on, and a `resize`
// when it is not the size last reported - whether the user or the script
// changed it, as on the other platforms.
static void On_Content_Allocate(GtkWidget *content, GtkAllocation *a, gpointer data)
{
	GUIWIN *win = (GUIWIN*)data;
	GTKWIN *gw;
	if (!win || !win->handle || !win->hob) return;
	gw = GW(win);
	if (a->width <= 1 && a->height <= 1) return;   // not laid out yet
	gw->want_w = a->width;
	gw->want_h = a->height;
	REBOL_GUI_BOX(gw->content)->nat_w = a->width;
	REBOL_GUI_BOX(gw->content)->nat_h = a->height;
	if (a->width != gw->seen_w || a->height != gw->seen_h) {
		gw->seen_w = a->width;
		gw->seen_h = a->height;
		Gui_Queue_Event(win->hob, EVT_RESIZE, a->width, a->height, 0);
	}
}

// A see-through window paints its own background: nothing at all.
static gboolean On_Window_Draw(GtkWidget *window, cairo_t *cr, gpointer data)
{
	GUIWIN *win = Window_Of_Gtk(window);
	if (win && GUI_BG_IS_CLEAR(win->background)) {
		cairo_save(cr);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_rgba(cr, 0, 0, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	return FALSE;
}

static void Close_Window(GUIWIN *win, REBOOL destroy);

/***********************************************************************
**  The window went away without `close-window` - its X window was
**  destroyed by someone else, and GTK destroys the widget with it. Every
**  handle still pointing into it is let go of, exactly as a close does.
***********************************************************************/
static void On_Window_Destroy(GtkWidget *window, gpointer data)
{
	GUIWIN *win = Window_Of_Gtk(window);
	if (win && win->handle && !GW(win)->closing) Close_Window(win, FALSE);
}

static void On_Style_Updated(GtkWidget *window, gpointer data)
{
	GUIWIN *win = Window_Of_Gtk(window);
	if (win && win->hob) Gui_Theme_Changed(win, Gui_Window_Dark(win));
}


static void Resize_To_Want(GUIWIN *win)
{
	GTKWIN *gw = GW(win);
	gint extra = 0;
	REBOL_GUI_BOX(gw->content)->nat_w = gw->want_w;
	REBOL_GUI_BOX(gw->content)->nat_h = gw->want_h;
	if (gw->menubar && gtk_widget_get_visible(gw->menubar)) {
		gint min = 0, nat = 0;
		gtk_widget_get_preferred_height(gw->menubar, &min, &nat);
		extra = nat;
	}
	gtk_widget_queue_resize(gw->content);
	gtk_window_resize(GTK_WINDOW(gw->window), gw->want_w > 0 ? gw->want_w : 1,
	                  (gw->want_h > 0 ? gw->want_h : 1) + extra);
}


REBOOL Gui_Open_Window(GUIWIN *win, REBINT x, REBINT y, REBINT w, REBINT h,
                       const REBYTE *title, REBCNT title_len, REBCNT flags)
{
	GTKWIN    *gw;
	GtkWidget *window;
	GdkScreen *screen;
	GdkVisual *visual;
	gchar     *name;

	if (!Gtk_Ready) return FALSE;

	gw = g_new0(GTKWIN, 1);
	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gw->window = window;
	g_object_set_data(G_OBJECT(window), KEY_WINDOW, win);
	gtk_style_context_add_class(gtk_widget_get_style_context(window), WINDOW_CLASS);

	// An RGBA visual where there is a compositor, so a window can be made
	// see-through later - the visual cannot be changed once it is realized.
	screen = gtk_widget_get_screen(window);
	visual = gdk_screen_get_rgba_visual(screen);
	if (visual && gdk_screen_is_composited(screen)) gtk_widget_set_visual(window, visual);

	name = Dup_Text(title, title_len);
	gtk_window_set_title(GTK_WINDOW(window), title ? name : "Rebol");
	g_free(name);

	gw->vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(window), gw->vbox);

	gw->content = New_Box(ROLE_CONTENT);
	REBOL_GUI_BOX(gw->content)->win = win;
	gtk_widget_set_hexpand(gw->content, TRUE);
	gtk_widget_set_vexpand(gw->content, TRUE);
	gtk_box_pack_end(GTK_BOX(gw->vbox), gw->content, TRUE, TRUE, 0);

	gw->accel = gtk_accel_group_new();
	gtk_window_add_accel_group(GTK_WINDOW(window), gw->accel);
	gw->items = g_ptr_array_new();

	gw->want_w = w; gw->want_h = h;
	gw->seen_w = w; gw->seen_h = h;
	gw->resizable = (flags & (GUI_WIN_FIXED | GUI_WIN_BORDERLESS)) ? FALSE : TRUE;
	gw->decorated = (flags & GUI_WIN_BORDERLESS) ? FALSE : TRUE;

	win->handle = (void*)gw;
	win->flags  = 0;

	gtk_window_set_resizable(GTK_WINDOW(window), gw->resizable);
	gtk_window_set_decorated(GTK_WINDOW(window), gw->decorated);

	// A dialog stays above its owner, and is not minimised on its own.
	if (win->modal_owner && ((GUIWIN*)win->modal_owner)->handle) {
		gtk_window_set_transient_for(GTK_WINDOW(window),
			GTKWINDOW((GUIWIN*)win->modal_owner));
		gtk_window_set_modal(GTK_WINDOW(window), TRUE);
		gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_DIALOG);
	}

	g_signal_connect(window, "delete-event", G_CALLBACK(On_Delete), NULL);
	g_signal_connect(window, "draw", G_CALLBACK(On_Window_Draw), NULL);
	g_signal_connect(window, "style-updated", G_CALLBACK(On_Style_Updated), NULL);
	g_signal_connect(window, "destroy", G_CALLBACK(On_Window_Destroy), NULL);
	g_signal_connect(gw->content, "size-allocate", G_CALLBACK(On_Content_Allocate), win);

	gtk_widget_add_events(window, GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK
	                            | GDK_ENTER_NOTIFY_MASK);

	Resize_To_Want(win);
	if (x == GUI_DEFAULT_POS || y == GUI_DEFAULT_POS) {
		gtk_window_set_position(GTK_WINDOW(window),
			win->modal_owner ? GTK_WIN_POS_CENTER_ON_PARENT : GTK_WIN_POS_CENTER);
	} else {
		gtk_window_move(GTK_WINDOW(window), x, y);
	}

	gtk_widget_show(gw->vbox);
	gtk_widget_show(gw->content);
	// Realized now, hidden or not: the size, the screen and every child's
	// place are then answerable before the window is first shown.
	gtk_widget_realize(window);

	Windows = g_list_append(Windows, win);

	if (flags & GUI_WIN_TRANSPARENT) {
		win->background = GUI_BG_CLEAR;
		Gui_Window_Set_Background(win);
	}
	return TRUE;
}


// Modal dialogs - see gui.h. Input to the blocked windows is dropped by
// Event_Hook; here the dialog is only brought forward.
void Gui_Apply_Modal(GUIWIN *top)
{
	if (top && top->handle && (top->flags & GUIW_VISIBLE))
		gtk_window_present(GTKWINDOW(top));
}


static void Detach_Control(GUIWIDGET *wid);

void Gui_Close_Window(GUIWIN *win)
{
	Close_Window(win, TRUE);
}

static void Close_Window(GUIWIN *win, REBOOL destroy)
{
	GTKWIN *gw;
	GUIWIDGET *wid;

	if (!win || !win->handle) return;
	gw = GW(win);
	gw->closing = TRUE;

	if (Press_Window == win) {
		Press_Window = NULL; Press_Source = NULL; Press_Buttons = 0; Press_Kind = 0;
	}

	// The controls go with the window; their signals are cut first, so
	// nothing they say on the way out reaches a handle being torn down.
	for (wid = (GUIWIDGET*)win->widgets; wid; wid = (GUIWIDGET*)wid->next) {
		if (wid->handle) {
			Detach_Control(wid);
			wid->handle = NULL;
		}
	}

	Windows = g_list_remove(Windows, win);
	g_object_set_data(G_OBJECT(gw->window), KEY_WINDOW, NULL);
	g_signal_handlers_disconnect_by_data(gw->content, win);
	REBOL_GUI_BOX(gw->content)->win = NULL;

	win->handle = NULL;
	win->flags &= ~GUIW_VISIBLE;
	win->menu = NULL;

	if (destroy) gtk_widget_destroy(gw->window);
	g_ptr_array_free(gw->items, TRUE);
	g_object_unref(gw->accel);
	g_free(gw);

	if (win->hob) Gui_Window_Closed(win->hob);
}


void Gui_Show_Window(GUIWIN *win, REBOOL show)
{
	GTKWIN *gw;
	if (!win || !win->handle) return;
	gw = GW(win);
	if (show) {
		gtk_widget_show(gw->window);
		gtk_window_present(GTK_WINDOW(gw->window));
		gw->shown = TRUE;
		win->flags |= GUIW_VISIBLE;
	} else {
		gtk_widget_hide(gw->window);
		win->flags &= ~GUIW_VISIBLE;
	}
}


/***********************************************************************
**  Dispatches everything waiting, without blocking.
**
**  Iterating the default main context is what runs GDK's event source
**  (and so Event_Hook) and GTK's frame clock, which lays out and paints -
**  so drawing, too, happens here and only here, as on macOS.
**
**  The count is what the hook saw, not the iterations: a paint is not a
**  message.
***********************************************************************/
REBCNT Gui_Pump(void)
{
	REBCNT before = Pump_Events;
	int guard = 0;

	if (!Gtk_Ready) return 0;
	while (g_main_context_iteration(NULL, FALSE) && ++guard < 1000) {}
	return Pump_Events - before;
}


REBOOL Gui_Get_Size(GUIWIN *win, REBINT *w, REBINT *h)
{
	if (!win || !win->handle) return FALSE;
	*w = GW(win)->want_w;
	*h = GW(win)->want_h;
	return TRUE;
}

REBOOL Gui_Get_Offset(GUIWIN *win, REBINT *x, REBINT *y)
{
	gint gx = 0, gy = 0;
	if (!win || !win->handle) return FALSE;
	gtk_window_get_position(GTKWINDOW(win), &gx, &gy);
	*x = gx;
	*y = gy;
	return TRUE;
}

REBOOL Gui_Set_Size(GUIWIN *win, REBINT w, REBINT h)
{
	if (!win || !win->handle) return FALSE;
	GW(win)->want_w = w;
	GW(win)->want_h = h;
	Resize_To_Want(win);
	return TRUE;
}

REBOOL Gui_Set_Offset(GUIWIN *win, REBINT x, REBINT y)
{
	if (!win || !win->handle) return FALSE;
	gtk_window_move(GTKWINDOW(win), x, y);
	return TRUE;
}


//-- the frame ----------------------------------------------------------------
//
// Read back from GTK rather than shadowed where GTK can say - it keeps
// both flags for a window. Changing either keeps the client size, which is
// what gtk_window_resize() is asked for anyway.

void Gui_Window_Apply_Border(GUIWIN *win)
{
	if (!win || !win->handle) return;
	gtk_widget_queue_draw(GW(win)->content);
}

REBOOL Gui_Get_Resizable(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;
	return gtk_window_get_resizable(GTKWINDOW(win)) ? TRUE : FALSE;
}

REBOOL Gui_Set_Resizable(GUIWIN *win, REBOOL on)
{
	if (!win || !win->handle) return FALSE;
	GW(win)->resizable = on;
	gtk_window_set_resizable(GTKWINDOW(win), on ? TRUE : FALSE);
	Resize_To_Want(win);
	return TRUE;
}

REBOOL Gui_Get_Title_Bar(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;
	return gtk_window_get_decorated(GTKWINDOW(win)) ? TRUE : FALSE;
}

REBOOL Gui_Set_Title_Bar(GUIWIN *win, REBOOL on)
{
	if (!win || !win->handle) return FALSE;
	GW(win)->decorated = on;
	gtk_window_set_decorated(GTKWINDOW(win), on ? TRUE : FALSE);
	Resize_To_Want(win);
	Gui_Window_Apply_Border(win);
	return TRUE;
}

REBSER* Gui_Get_Title(GUIWIN *win)
{
	if (!win || !win->handle) return NULL;
	return To_Rebol(gtk_window_get_title(GTKWINDOW(win)));
}

REBOOL Gui_Set_Title(GUIWIN *win, const REBYTE *utf8, REBCNT len)
{
	gchar *name;
	if (!win || !win->handle) return FALSE;
	name = Dup_Text(utf8, len);
	gtk_window_set_title(GTKWINDOW(win), name);
	g_free(name);
	return TRUE;
}


/***********************************************************************
**  The window's background: a colour is painted by the content box, the
**  platform's own is GtkWindow's, and see-through is a window which
**  paints nothing - on an RGBA visual, which a compositor then blends.
***********************************************************************/
void Gui_Window_Set_Background(GUIWIN *win)
{
	GtkWidget *window;
	if (!win || !win->handle) return;
	window = GW(win)->window;
	gtk_widget_set_app_paintable(window, GUI_BG_IS_CLEAR(win->background) ? TRUE : FALSE);
	gtk_widget_queue_draw(window);
}


//== menus ====================================================================
//
// A window's menu bar is a GtkMenuBar above its client area. Adding one
// grows the window by the bar's height, so the client size is what it was -
// the same promise the Windows backend makes.

static void On_Menu_Item(GtkMenuItem *item, gpointer data)
{
	GUIWIN *win = (GUIWIN*)data;
	REBCNT id = (REBCNT)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), KEY_ITEM));
	if (Popup_Tracking) {
		Popup_Pick = id;
		return;
	}
	if (win && win->handle && !Gui_Window_Blocked(win)) Gui_Menu_Picked(win, id);
}

REBOOL Gui_Menu_Begin(GUIWIN *win)
{
	GTKWIN *gw;
	if (!win || !win->handle) return FALSE;
	gw = GW(win);
	gw->menubar = gtk_menu_bar_new();
	gtk_box_pack_start(GTK_BOX(gw->vbox), gw->menubar, FALSE, FALSE, 0);
	win->menu = (void*)gw->menubar;
	return TRUE;
}

static GtkWidget *Menu_Shell(GUIWIN *win, void *parent)
{
	if (parent) return GTK_WIDGET(parent);
	return win && win->handle ? GW(win)->menubar : NULL;
}

// Whether a menu belongs to the bar, where shortcuts work, rather than
// to a context menu.
static REBOOL Menu_Is_Live(GUIWIN *win, void *parent)
{
	if (!parent) return TRUE;
	return GTK_IS_MENU(parent)
	    && gtk_menu_get_accel_group(GTK_MENU(parent)) == GW(win)->accel;
}

void* Gui_Menu_Add_Popup(GUIWIN *win, void *parent, const REBYTE *label, REBCNT len)
{
	GtkWidget *shell = Menu_Shell(win, parent);
	GtkWidget *item, *menu;
	gchar *text, *mn;

	if (!shell) return NULL;
	text = Dup_Text(label, len);
	mn = To_Mnemonic(text);
	item = gtk_menu_item_new_with_mnemonic(mn);
	g_free(mn);
	g_free(text);

	menu = gtk_menu_new();
	if (Menu_Is_Live(win, parent)) gtk_menu_set_accel_group(GTK_MENU(menu), GW(win)->accel);
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(shell), item);
	gtk_widget_show(item);
	return (void*)menu;
}

void Gui_Menu_Add_Item(GUIWIN *win, void *parent, const REBYTE *label, REBCNT len,
                       REBCNT item_id, REBCNT key, REBCNT mods)
{
	GtkWidget *shell = Menu_Shell(win, parent);
	GtkWidget *item;
	GTKWIN *gw;
	gchar *text, *mn;

	if (!shell) return;
	gw = GW(win);
	text = Dup_Text(label, len);
	mn = To_Mnemonic(text);
	item = gtk_menu_item_new_with_mnemonic(mn);
	g_free(mn);
	g_free(text);

	g_object_set_data(G_OBJECT(item), KEY_ITEM, GUINT_TO_POINTER(item_id));
	g_signal_connect(item, "activate", G_CALLBACK(On_Menu_Item), win);

	if (key) {
		GdkModifierType m = GDK_CONTROL_MASK;
		guint k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
		if (mods & GUI_FLAG_SHIFT)   m |= GDK_SHIFT_MASK;
		if (mods & GUI_FLAG_ALT)     m |= GDK_MOD1_MASK;
		k = gdk_unicode_to_keyval(k);
		// On the bar the shortcut works; in a context menu it is only a
		// label, since the menu is not there to hear it. A context menu is
		// the one kind of menu without the window's accelerator group.
		if (Menu_Is_Live(win, parent))
			gtk_widget_add_accelerator(item, "activate", gw->accel, k, m, GTK_ACCEL_VISIBLE);
		else {
			GtkWidget *child = gtk_bin_get_child(GTK_BIN(item));
			if (GTK_IS_ACCEL_LABEL(child)) gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), k, m);
		}
	}

	if (item_id > 0) {
		if (gw->items->len < item_id) g_ptr_array_set_size(gw->items, (guint)item_id);
		g_ptr_array_index(gw->items, item_id - 1) = item;
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(shell), item);
	gtk_widget_show(item);
}

void Gui_Menu_Add_Separator(GUIWIN *win, void *parent)
{
	GtkWidget *shell = Menu_Shell(win, parent);
	GtkWidget *sep;
	if (!shell) return;
	sep = gtk_separator_menu_item_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(shell), sep);
	gtk_widget_show(sep);
}

REBOOL Gui_Menu_End(GUIWIN *win)
{
	if (!win || !win->handle || !GW(win)->menubar) return FALSE;
	gtk_widget_show(GW(win)->menubar);
	Resize_To_Want(win);
	return TRUE;
}

void Gui_Menu_Free(GUIWIN *win)
{
	GTKWIN *gw;
	if (!win || !win->handle) return;
	gw = GW(win);
	win->menu = NULL;
	if (!gw->menubar) return;
	gtk_widget_destroy(gw->menubar);
	gw->menubar = NULL;
	g_ptr_array_set_size(gw->items, 0);
	Resize_To_Want(win);
}

void Gui_Menu_Enable(GUIWIN *win, REBCNT item_id, REBOOL enabled)
{
	GTKWIN *gw;
	GtkWidget *item;
	if (!win || !win->handle || item_id == 0) return;
	gw = GW(win);
	if (item_id > gw->items->len) return;
	item = (GtkWidget*)g_ptr_array_index(gw->items, item_id - 1);
	if (item) gtk_widget_set_sensitive(item, enabled ? TRUE : FALSE);
}


/***********************************************************************
**  Context menus - see gui.h. The menu is shown and a main loop of its
**  own runs until it closes; a pick is recorded by On_Menu_Item while
**  Popup_Tracking is set. GTK emits `deactivate` before the item's
**  `activate`, in the same dispatch, so the loop ends after both.
***********************************************************************/
void* Gui_Popup_Begin(GUIWIN *win)
{
	GtkWidget *menu;
	if (!win || !win->handle) return NULL;
	menu = gtk_menu_new();
	g_object_ref_sink(menu);
	return (void*)menu;
}

static void On_Popup_Done(GtkMenuShell *menu, gpointer loop)
{
	g_main_loop_quit((GMainLoop*)loop);
}

REBCNT Gui_Popup_Track(GUIWIN *win, void *root, REBOOL at, REBINT x, REBINT y)
{
	GtkMenu   *menu;
	GMainLoop *loop;
	GdkWindow *gw;
	GdkEvent  *trigger, *made = NULL;
	gulong     done;

	if (!win || !win->handle || !root) return 0;
	menu = GTK_MENU(root);
	gw = Content_Window(win);
	if (!gw) return 0;

	gtk_menu_attach_to_widget(menu, GW(win)->content, NULL);
	loop = g_main_loop_new(NULL, FALSE);
	trigger = Last_Press;
	if (!trigger || Window_Of_Gdk(trigger->any.window) != win) {
		// Opened from code, with no press of ours to answer: an event is
		// made up, on the window and the pointer, which is all GTK asks of
		// one.
		GdkDisplay *display = gdk_window_get_display(gw);
		made = gdk_event_new(GDK_BUTTON_PRESS);
		made->button.window = g_object_ref(gw);
		made->button.time   = GDK_CURRENT_TIME;
		made->button.button = 3;
		gdk_event_set_device(made, gdk_seat_get_pointer(gdk_display_get_default_seat(display)));
		trigger = made;
	}
	done = g_signal_connect(menu, "deactivate", G_CALLBACK(On_Popup_Done), loop);

	Popup_Pick = 0;
	Popup_Tracking = TRUE;
	if (at) {
		GdkRectangle r;
		r.x = x; r.y = y; r.width = 1; r.height = 1;
		gtk_menu_popup_at_rect(menu, gw, &r, GDK_GRAVITY_NORTH_WEST,
		                       GDK_GRAVITY_NORTH_WEST, trigger);
	} else {
		GdkDisplay *display = gdk_window_get_display(gw);
		GdkDevice  *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
		gint px = 0, py = 0;
		GdkRectangle r;
		gdk_window_get_device_position(gw, pointer, &px, &py, NULL);
		r.x = px; r.y = py; r.width = 1; r.height = 1;
		gtk_menu_popup_at_rect(menu, gw, &r, GDK_GRAVITY_NORTH_WEST,
		                       GDK_GRAVITY_NORTH_WEST, trigger);
	}
	if (made) gdk_event_free(made);
	if (gtk_widget_get_visible(GTK_WIDGET(menu))) g_main_loop_run(loop);
	Popup_Tracking = FALSE;

	g_signal_handler_disconnect(menu, done);
	g_main_loop_unref(loop);
	gtk_menu_detach(menu);
	return Popup_Pick;
}

void Gui_Popup_Free(GUIWIN *win, void *root)
{
	(void)win;
	if (!root) return;
	gtk_widget_destroy(GTK_WIDGET(root));
	g_object_unref(root);
}


//== scale and screens ========================================================

REBDEC Gui_Get_Scale(GUIWIN *win)
{
	GdkMonitor *mon = NULL;
	GdkDisplay *display = gdk_display_get_default();

	if (win && win->handle)
		return (REBDEC)gtk_widget_get_scale_factor(GW(win)->window);
	if (display) mon = gdk_display_get_primary_monitor(display);
	if (!mon && display && gdk_display_get_n_monitors(display) > 0)
		mon = gdk_display_get_monitor(display, 0);
	return mon ? (REBDEC)gdk_monitor_get_scale_factor(mon) : 1.0;
}

/***********************************************************************
**  A display is named by its connector - DP-1, HDMI-2 - which GDK
**  reports as the monitor's model on X11 and keeps while the display
**  stays connected. Where that is not available, by its position in the
**  list. The primary one comes first.
***********************************************************************/
static void Monitor_Key(GdkMonitor *mon, int index, REBYTE *key)
{
	const char *model = gdk_monitor_get_model(mon);
	if (model && *model) snprintf((char*)key, GUI_SCREEN_KEY, "%s", model);
	else snprintf((char*)key, GUI_SCREEN_KEY, "monitor-%d", index);
}

static GdkMonitor *Primary_Monitor(GdkDisplay *display)
{
	GdkMonitor *mon = gdk_display_get_primary_monitor(display);
	if (!mon && gdk_display_get_n_monitors(display) > 0)
		mon = gdk_display_get_monitor(display, 0);
	return mon;
}

static GdkMonitor *Find_Monitor(const REBYTE *key, REBOOL *primary)
{
	GdkDisplay *display = gdk_display_get_default();
	REBYTE k[GUI_SCREEN_KEY];
	int n, count;
	if (!display || !key) return NULL;
	count = gdk_display_get_n_monitors(display);
	for (n = 0; n < count; n++) {
		GdkMonitor *mon = gdk_display_get_monitor(display, n);
		Monitor_Key(mon, n, k);
		if (strcmp((const char*)k, (const char*)key) == 0) {
			if (primary) *primary = (mon == Primary_Monitor(display));
			return mon;
		}
	}
	return NULL;
}

static int Monitor_Index(GdkMonitor *mon)
{
	GdkDisplay *display = gdk_monitor_get_display(mon);
	int n, count = gdk_display_get_n_monitors(display);
	for (n = 0; n < count; n++)
		if (gdk_display_get_monitor(display, n) == mon) return n;
	return 0;
}

REBCNT Gui_Screen_Keys(REBYTE (*keys)[GUI_SCREEN_KEY], REBCNT max)
{
	GdkDisplay *display = gdk_display_get_default();
	GdkMonitor *primary;
	int n, count;
	REBCNT out = 0;

	if (!Gtk_Ready || !display) return 0;
	count = gdk_display_get_n_monitors(display);
	primary = Primary_Monitor(display);
	if (primary && out < max) Monitor_Key(primary, Monitor_Index(primary), keys[out++]);
	for (n = 0; n < count; n++) {
		GdkMonitor *mon = gdk_display_get_monitor(display, n);
		if (mon == primary) continue;
		if (out < max) Monitor_Key(mon, n, keys[out]);
		out++;
	}
	return (REBCNT)count;
}

REBOOL Gui_Screen_Info(const REBYTE *key, GUISCREENINFO *info)
{
	REBOOL primary = FALSE;
	GdkMonitor *mon;
	GdkRectangle r;

	if (!Gtk_Ready || !key || !info || !(mon = Find_Monitor(key, &primary))) return FALSE;
	gdk_monitor_get_geometry(mon, &r);
	info->x = r.x; info->y = r.y; info->w = r.width; info->h = r.height;
	// Without panels and docks - where a window can go.
	gdk_monitor_get_workarea(mon, &r);
	info->wx = r.x; info->wy = r.y; info->ww = r.width; info->wh = r.height;
	info->scale   = (REBDEC)gdk_monitor_get_scale_factor(mon);
	info->primary = primary;
	return TRUE;
}

REBSER* Gui_Screen_Name(const REBYTE *key)
{
	GdkMonitor *mon;
	const char *maker, *model;
	gchar *name;
	REBSER *out;

	if (!Gtk_Ready || !key || !(mon = Find_Monitor(key, NULL))) return NULL;
	maker = gdk_monitor_get_manufacturer(mon);
	model = gdk_monitor_get_model(mon);
	if (maker && *maker && model && *model) name = g_strdup_printf("%s %s", maker, model);
	else if (model && *model) name = g_strdup(model);
	else name = g_strdup_printf("Display %s", (const char*)key);
	out = To_Rebol(name);
	g_free(name);
	return out;
}

REBOOL Gui_Window_Screen(GUIWIN *win, REBYTE *key)
{
	GdkWindow *gw;
	GdkMonitor *mon;
	if (!win || !win->handle || !key) return FALSE;
	gw = gtk_widget_get_window(GW(win)->window);
	if (!gw) return FALSE;
	mon = gdk_display_get_monitor_at_window(gdk_window_get_display(gw), gw);
	if (!mon) return FALSE;
	Monitor_Key(mon, Monitor_Index(mon), key);
	return TRUE;
}

// Where the pointer is on the desktop, for `track-mouse`. See gui.h.
REBOOL Gui_Pointer_At(REBYTE *key, REBINT *x, REBINT *y, REBINT *mods, REBOOL *ours)
{
	GdkDisplay *display = gdk_display_get_default();
	GdkDevice  *pointer;
	GdkScreen  *screen = NULL;
	GdkMonitor *mon;
	GdkRectangle r;
	GdkKeymap  *keymap;
	gint px = 0, py = 0, wx, wy;

	*ours = FALSE;
	if (!Gtk_Ready || !display) return FALSE;

	// A press which started in one of our windows is still that window's.
	if (Press_Buttons) { *ours = TRUE; return TRUE; }

	pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
	if (!pointer) return FALSE;
	if (gdk_device_get_window_at_position(pointer, &wx, &wy)) {
		*ours = TRUE;   // one of this process's windows is under it
		return TRUE;
	}
	gdk_device_get_position(pointer, &screen, &px, &py);
	mon = gdk_display_get_monitor_at_point(display, px, py);
	if (!mon) return FALSE;

	Monitor_Key(mon, Monitor_Index(mon), key);
	gdk_monitor_get_geometry(mon, &r);
	*x = px - r.x;
	*y = py - r.y;
	keymap = gdk_keymap_get_for_display(display);
	*mods = keymap ? Modifier_Bits(gdk_keymap_get_modifier_state(keymap)) : 0;
	return TRUE;
}


//== light and dark ===========================================================
//
// GTK's controls follow the theme by themselves, so `dark-controls?` has
// nothing to do here, as on macOS. Whether a window is dark is read off
// what the theme gives it: text lighter than it is dark means a dark
// theme, whatever the theme is called.

void Gui_Window_Dark_Controls(GUIWIN *win, REBOOL on)
{
	(void)win; (void)on;
}

REBOOL Gui_Window_Dark(GUIWIN *win)
{
	GtkStyleContext *ctx;
	GdkRGBA fg;
	if (!win || !win->handle) return FALSE;
	ctx = gtk_widget_get_style_context(GW(win)->window);
	gtk_style_context_get_color(ctx, GTK_STATE_FLAG_NORMAL, &fg);
	return (0.299 * fg.red + 0.587 * fg.green + 0.114 * fg.blue) > 0.5 ? TRUE : FALSE;
}

// The shared layer reports a real change only, so every window is told.
static void Theme_Notify(GObject *settings, GParamSpec *spec, gpointer data)
{
	GList *l;
	for (l = Windows; l; l = l->next) {
		GUIWIN *win = (GUIWIN*)l->data;
		if (win && win->hob && win->handle) Gui_Theme_Changed(win, Gui_Window_Dark(win));
	}
}


//== widgets: common ==========================================================

/***********************************************************************
**  Signals are connected with the GUIWIDGET as their data, and every
**  object connected to is remembered, so that Detach_Control() can cut
**  them all before the handle goes - an inner text view, a selection, a
**  buffer, a combo box's entry all say things of their own.
***********************************************************************/
#define KEY_CONNECTED "rebol-gui-connected"

static void Connect(GUIWIDGET *wid, gpointer object, const char *signal, GCallback cb)
{
	GPtrArray *list;
	GtkWidget *outer = GTKW(wid);
	g_signal_connect(object, signal, cb, wid);
	if (!outer) return;
	list = (GPtrArray*)g_object_get_data(G_OBJECT(outer), KEY_CONNECTED);
	if (!list) {
		list = g_ptr_array_new_with_free_func(g_object_unref);
		g_object_set_data_full(G_OBJECT(outer), KEY_CONNECTED, list,
		                       (GDestroyNotify)g_ptr_array_unref);
	}
	if (!g_ptr_array_find(list, object, NULL))
		g_ptr_array_add(list, g_object_ref(object));
}

static void Detach_Control(GUIWIDGET *wid)
{
	GtkWidget *outer, *inner, *partner;
	GPtrArray *list;
	guint n;

	if (!wid || !wid->handle) return;
	outer = GTKW(wid);
	inner = Inner_Of(wid);

	if (Press_Source && Press_Source == wid->hob) {
		Press_Source = NULL; Press_Kind = 0;
	}

	list = (GPtrArray*)g_object_get_data(G_OBJECT(outer), KEY_CONNECTED);
	if (list) for (n = 0; n < list->len; n++)
		g_signal_handlers_disconnect_by_data(g_ptr_array_index(list, n), wid);

	// A list-view asks the widget for its cells; without a model it asks
	// nothing more.
	if (wid->kind == W_GUI_WIDGET_LIST_VIEW && inner)
		gtk_tree_view_set_model(GTK_TREE_VIEW(inner), NULL);

	if (IS_BOX(outer)) REBOL_GUI_BOX(outer)->wid = NULL;
	g_object_set_data(G_OBJECT(outer), KEY_WIDGET, NULL);
	if (inner) g_object_set_data(G_OBJECT(inner), KEY_WIDGET, NULL);

	partner = (GtkWidget*)g_object_get_data(G_OBJECT(outer), KEY_PARTNER);
	if (partner) g_object_set_data(G_OBJECT(outer), KEY_PARTNER, NULL);
}

// Queues an event whose position is the control's own offset: a signal
// comes with no pointer position, and this is what the other platforms
// report for a click on a button or an edit in a field.
static void Queue_Widget(GUIWIDGET *wid, REBCNT type)
{
	REBINT x = 0, y = 0, w, h;
	if (!wid || !wid->hob) return;
	Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
	Gui_Queue_Event(wid->hob, type, x, y, Current_Modifiers());
}

// The box a new control goes into: the panel (or page, or image widget)
// holding it, or the window's client area. `wid->parent` is set before
// any creation call.
static GtkWidget *Parent_Box(GUIWIDGET *wid, GUIWIN *owner)
{
	if (wid->parent && ((GUIWIDGET*)wid->parent)->handle
	    && IS_BOX(((GUIWIDGET*)wid->parent)->handle))
		return GTKW((GUIWIDGET*)wid->parent);
	return GW(owner)->content;
}

static REBOOL Place(GUIWIDGET *wid, GUIWIN *owner, GtkWidget *w,
                    REBINT x, REBINT y, REBINT width, REBINT height)
{
	GtkWidget *box = Parent_Box(wid, owner);
	if (!box) {
		gtk_widget_destroy(w);
		return FALSE;
	}
	wid->handle = (void*)w;
	g_object_set_data(G_OBJECT(w), KEY_WIDGET, wid);
	gtk_widget_add_events(w, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
	                       | GDK_BUTTON_RELEASE_MASK);
	Box_Put(box, w, x, y, width, height);
	gtk_widget_show_all(w);
	return TRUE;
}

static gboolean On_Focus_In(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	Queue_Widget((GUIWIDGET*)data, EVT_FOCUS);
	return FALSE;
}

static gboolean On_Focus_Out(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	Queue_Widget((GUIWIDGET*)data, EVT_UNFOCUS);
	return FALSE;
}

static void Connect_Focus(GUIWIDGET *wid, GtkWidget *target)
{
	Connect(wid, target, "focus-in-event", G_CALLBACK(On_Focus_In));
	Connect(wid, target, "focus-out-event", G_CALLBACK(On_Focus_Out));
}

static void On_Changed(gpointer object, gpointer data)
{
	if (Quiet) return;
	Queue_Widget((GUIWIDGET*)data, EVT_CHANGE);
}

static void On_Activate(gpointer object, gpointer data)
{
	Queue_Widget((GUIWIDGET*)data, EVT_CLICK);
}


//== typography and colour ====================================================
//
// All three are CSS, as GTK 3 wants: each widget has a provider of its own
// for its font and text colour, added to its style context and to those of
// its inner parts (a button's label, a combo box's cell), because colour
// set on a button is not inherited by a label the theme colours itself.
// A second provider carries the background, on the one node which draws it.

typedef struct {
	GtkCssProvider *text;
	GtkCssProvider *back;
	gchar  *family;   // NULL for the theme's
	REBINT  size;     // points; 0 for the theme's
	REBCNT  style;    // GUI_FONT_*
	REBOOL  has_font;
} WSTYLE;

static void Free_Style(gpointer p)
{
	WSTYLE *s = (WSTYLE*)p;
	if (s->text) g_object_unref(s->text);
	if (s->back) g_object_unref(s->back);
	g_free(s->family);
	g_free(s);
}

static WSTYLE *Style_Of(GUIWIDGET *wid)
{
	WSTYLE *s = (WSTYLE*)g_object_get_data(G_OBJECT(wid->handle), KEY_STYLE);
	if (!s) {
		s = g_new0(WSTYLE, 1);
		g_object_set_data_full(G_OBJECT(wid->handle), KEY_STYLE, s, Free_Style);
	}
	return s;
}

// Which widget carries the text: the text view of an area, the tree view
// of a list, the control itself for everything else.
static GtkWidget *Text_Target(GUIWIDGET *wid)
{
	if (wid->kind == W_GUI_WIDGET_AREA || IS_TABLE(wid)) return Inner_Of(wid);
	return GTKW(wid);
}

static void Add_Provider_To(GtkWidget *w, gpointer provider);

static void Add_Provider_Walk(GtkWidget *w, gpointer provider)
{
	Add_Provider_To(w, provider);
}

static void Add_Provider_To(GtkWidget *w, gpointer provider)
{
	GtkStyleContext *ctx = gtk_widget_get_style_context(w);
	if (g_object_get_data(G_OBJECT(w), KEY_PROV) != provider) {
		gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(provider),
		                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
		g_object_set_data(G_OBJECT(w), KEY_PROV, provider);
	}
	// Into the parts of the control, but not into another widget's
	// control: a panel's children style themselves.
	if (GTK_IS_CONTAINER(w) && !IS_BOX(w))
		gtk_container_forall(GTK_CONTAINER(w), Add_Provider_Walk, provider);
}

/***********************************************************************
**  GTK recomputes styles lazily, at the next frame, and a label keeps
**  the text layout it made with the old font until then - so a size
**  measured, or a font read back, right after a change would be the old
**  one. The style is reset, and each widget told its style changed, so
**  what it cached goes now.
**
**  The theme's transitions are switched off on a styled widget for the
**  same reason: Adwaita animates a button's properties, its font size
**  included, and the value read back mid-animation is the old one.
***********************************************************************/
static void Style_Changed_Walk(GtkWidget *w, gpointer data)
{
	g_signal_emit_by_name(w, "style-updated");
	if (GTK_IS_CONTAINER(w) && !IS_BOX(w))
		gtk_container_forall(GTK_CONTAINER(w), Style_Changed_Walk, data);
}

static void Apply_Text_Style(GUIWIDGET *wid)
{
	WSTYLE  *s;
	GString *css;
	GtkWidget *target;

	if (!wid || !wid->handle) return;
	s = Style_Of(wid);
	target = Text_Target(wid);
	if (!target) return;

	css = g_string_new(NULL);
	if (s->has_font || GUI_COLOR_HAS(wid->color))
		g_string_append(css, "* { transition: none; }\n");
	if (s->has_font) {
		g_string_append(css, "* {");
		if (s->family) {
			gchar *esc = g_strescape(s->family, NULL);
			g_string_append_printf(css, " font-family: \"%s\";", esc);
			g_free(esc);
		}
		if (s->size > 0) g_string_append_printf(css, " font-size: %dpt;", (int)s->size);
		g_string_append_printf(css, " font-weight: %s;", (s->style & GUI_FONT_BOLD) ? "bold" : "normal");
		g_string_append_printf(css, " font-style: %s;", (s->style & GUI_FONT_ITALIC) ? "italic" : "normal");
		g_string_append(css, " }\n");
	}
	if (GUI_COLOR_HAS(wid->color)) {
		// A selected row, and selected text, keep the theme's contrast.
		g_string_append_printf(css, "*:not(:selected):not(selection) { color: #%02x%02x%02x; }\n",
			(unsigned)GUI_COLOR_R(wid->color), (unsigned)GUI_COLOR_G(wid->color),
			(unsigned)GUI_COLOR_B(wid->color));
	}

	if (!s->text) s->text = gtk_css_provider_new();
	gtk_css_provider_load_from_data(s->text, css->str, -1, NULL);
	g_string_free(css, TRUE);
	Add_Provider_To(target, s->text);
	if (target != GTKW(wid)) Add_Provider_To(GTKW(wid), s->text);
	gtk_widget_reset_style(GTKW(wid));
	Style_Changed_Walk(GTKW(wid), NULL);
	gtk_widget_queue_resize(GTKW(wid));
}

// The node which paints the widget's background, as a CSS selector
// relative to the widget it is added to.
static void Apply_Background(GUIWIDGET *wid)
{
	WSTYLE  *s;
	GtkWidget *target = GTKW(wid);
	const char *selector = NULL;
	gchar *css;
	gchar color[32];

	if (!wid || !wid->handle) return;
	s = Style_Of(wid);

	switch (wid->kind) {
	case W_GUI_WIDGET_PANEL:
	case W_GUI_WIDGET_IMAGE:
		// Painted by the box itself.
		gtk_widget_queue_draw(target);
		return;
	case W_GUI_WIDGET_AREA:
		target = Inner_Of(wid);
		selector = "textview text";
		break;
	case W_GUI_WIDGET_TEXT_LIST:
	case W_GUI_WIDGET_LIST_VIEW:
	case W_GUI_WIDGET_TREE_VIEW:
		target = Inner_Of(wid);
		selector = "treeview.view:not(:selected)";
		break;
	default:
		selector = gtk_widget_class_get_css_name(GTK_WIDGET_GET_CLASS(target));
		break;
	}
	if (!target || !selector) return;

	if (GUI_COLOR_HAS(wid->background)) {
		snprintf(color, sizeof(color), "#%02x%02x%02x",
		         (unsigned)GUI_COLOR_R(wid->background),
		         (unsigned)GUI_COLOR_G(wid->background),
		         (unsigned)GUI_COLOR_B(wid->background));
	}
	else if (GUI_BG_IS_CLEAR(wid->background)
	         && wid->kind != W_GUI_WIDGET_FIELD && wid->kind != W_GUI_WIDGET_AREA
	         && !IS_TABLE(wid)) {
		// An entry keeps its fill: what would be left is a frame round
		// nothing, which is also what the other platforms decided.
		strcpy(color, "transparent");
	}
	else color[0] = 0;

	if (!s->back) {
		s->back = gtk_css_provider_new();
		gtk_style_context_add_provider(gtk_widget_get_style_context(target),
			GTK_STYLE_PROVIDER(s->back), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	css = color[0]
		? g_strdup_printf("%s { background-color: %s; background-image: none; }", selector, color)
		: g_strdup("");
	gtk_css_provider_load_from_data(s->back, css, -1, NULL);
	g_free(css);
	gtk_widget_queue_draw(GTKW(wid));
}


REBOOL Gui_Widget_Get_Font(GUIWIDGET *wid, REBSER **name, REBINT *size, REBCNT *style)
{
	GtkWidget *target;
	GtkStyleContext *ctx;
	PangoFontDescription *desc = NULL;

	if (name)  *name  = NULL;
	if (size)  *size  = 0;
	if (style) *style = 0;
	if (!wid || !wid->handle || !(target = Text_Target(wid))) return FALSE;

	ctx = gtk_widget_get_style_context(target);
	gtk_style_context_get(ctx, gtk_style_context_get_state(ctx),
	                      GTK_STYLE_PROPERTY_FONT, &desc, NULL);
	if (!desc) return FALSE;

	if (name) {
		// The FAMILY - "DejaVu Sans" - and the first of a list of them.
		const char *family = pango_font_description_get_family(desc);
		if (family) {
			gchar *first = g_strdup(family);
			gchar *comma = strchr(first, ',');
			if (comma) *comma = 0;
			*name = To_Rebol(g_strstrip(first));
			g_free(first);
		}
	}
	if (size) {
		gint sz = pango_font_description_get_size(desc);
		gdouble pt = (gdouble)sz / PANGO_SCALE;
		// An absolute size is in device units, 96 to the inch.
		if (pango_font_description_get_size_is_absolute(desc)) pt = pt * 72.0 / 96.0;
		*size = (REBINT)(pt + 0.5);
	}
	if (style) {
		if (pango_font_description_get_weight(desc) >= PANGO_WEIGHT_BOLD) *style |= GUI_FONT_BOLD;
		if (pango_font_description_get_style(desc) != PANGO_STYLE_NORMAL) *style |= GUI_FONT_ITALIC;
	}
	pango_font_description_free(desc);
	return TRUE;
}

REBOOL Gui_Widget_Set_Font(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len,
                           REBINT size, REBCNT style)
{
	WSTYLE *s;
	if (!wid || !wid->handle || !Text_Target(wid)) return FALSE;
	s = Style_Of(wid);
	g_free(s->family);
	s->family = (utf8 && len > 0) ? Dup_Text(utf8, len) : NULL;
	s->size   = size > 0 ? size : 0;
	s->style  = style;
	s->has_font = (s->family || s->size || s->style) ? TRUE : FALSE;
	Apply_Text_Style(wid);
	return TRUE;
}

REBOOL Gui_Widget_Set_Color(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	if (wid->kind == W_GUI_WIDGET_PANEL) {
		// Read by the panel's own paint.
		gtk_widget_queue_draw(GTKW(wid));
		return TRUE;
	}
	Apply_Text_Style(wid);
	if (IS_TABLE(wid)) gtk_widget_queue_draw(Inner_Of(wid));
	return TRUE;
}

void Gui_Widget_Set_Background(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;
	Apply_Background(wid);
	if (IS_TABLE(wid)) gtk_widget_queue_draw(Inner_Of(wid));
}


//== buttons ==================================================================

static void On_Clicked(GtkButton *button, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	REBINT x = 0, y = 0, w, h;
	if (Quiet || !wid || !wid->hob) return;
	// No pointer position comes with a click, so the position slot carries
	// the control's own offset - the same as the other platforms.
	Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
	Gui_Widget_Activated(wid, x, y, Current_Modifiers());
}

REBOOL Gui_Create_Button_Control(GUIWIDGET *wid, GUIWIN *owner,
                                 REBINT x, REBINT y, REBINT w, REBINT h,
                                 const REBYTE *text, REBCNT len)
{
	GtkWidget *button;
	gchar *label, *mn;

	if (!wid || !owner || !owner->handle) return FALSE;
	label = Dup_Text(text, len);
	mn = To_Mnemonic(label);
	g_free(label);

	switch (wid->kind) {
	case W_GUI_WIDGET_CHECK:
		button = gtk_check_button_new_with_mnemonic(mn);
		break;
	case W_GUI_WIDGET_TOGGLE:
		button = gtk_toggle_button_new_with_mnemonic(mn);
		break;
	case W_GUI_WIDGET_RADIO: {
		/***************************************************************
		**  Radio groups are the extension's own, so every radio is in
		**  a GTK group of two: itself and a hidden partner which is
		**  never shown. Switching the partner on is how a radio is
		**  switched off - GTK will not untick a radio on its own, and
		**  a real group would clear radios this extension did not ask
		**  it to.
		***************************************************************/
		GtkWidget *partner;
		button  = gtk_radio_button_new_with_mnemonic(NULL, mn);
		partner = gtk_radio_button_new_from_widget(GTK_RADIO_BUTTON(button));
		g_object_ref_sink(partner);
		g_object_set_data_full(G_OBJECT(button), KEY_PARTNER, partner,
		                       (GDestroyNotify)gtk_widget_destroy);
		Quiet++;
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(partner), TRUE);
		Quiet--;
		break; }
	default:
		button = gtk_button_new_with_mnemonic(mn);
		break;
	}
	g_free(mn);

	if (!Place(wid, owner, button, x, y, w, h)) return FALSE;
	Connect(wid, button, "clicked", G_CALLBACK(On_Clicked));
	return TRUE;
}

REBOOL Gui_Widget_Get_State(GUIWIDGET *wid)
{
	if (!wid || !wid->handle || !GTK_IS_TOGGLE_BUTTON(wid->handle)) return FALSE;
	return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(wid->handle)) ? TRUE : FALSE;
}

void Gui_Widget_Set_State(GUIWIDGET *wid, REBOOL on)
{
	GtkToggleButton *button;
	if (!wid || !wid->handle || !GTK_IS_TOGGLE_BUTTON(wid->handle)) return;
	button = GTK_TOGGLE_BUTTON(wid->handle);
	// Already there: nothing to do - the shared layer re-asserts every
	// radio of a group on every click.
	if ((gtk_toggle_button_get_active(button) ? TRUE : FALSE) == (on ? TRUE : FALSE)) return;
	Quiet++;
	if (wid->kind == W_GUI_WIDGET_RADIO && !on) {
		GtkWidget *partner = (GtkWidget*)g_object_get_data(G_OBJECT(button), KEY_PARTNER);
		if (partner) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(partner), TRUE);
	} else {
		gtk_toggle_button_set_active(button, on ? TRUE : FALSE);
	}
	Quiet--;
}


//== text =====================================================================

REBOOL Gui_Create_Text_Control(GUIWIDGET *wid, GUIWIN *owner,
                               REBINT x, REBINT y, REBINT w, REBINT h,
                               const REBYTE *text, REBCNT len)
{
	gchar *value;

	if (!wid || !owner || !owner->handle) return FALSE;
	value = Dup_Text(text, len);

	if (wid->kind == W_GUI_WIDGET_AREA) {
		GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
		GtkWidget *tv = gtk_text_view_new();
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));

		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
		// Tab moves on, as out of every other control, rather than typing
		// a tab into the text.
		gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(tv), FALSE);
		gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 3);
		gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 3);
		gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 2);
		gtk_text_buffer_set_text(buffer, value, -1);
		gtk_container_add(GTK_CONTAINER(scroll), tv);
		g_object_set_data(G_OBJECT(scroll), KEY_INNER, tv);
		g_object_set_data(G_OBJECT(tv), KEY_WIDGET, wid);
		g_free(value);

		if (!Place(wid, owner, scroll, x, y, w, h)) return FALSE;
		Connect(wid, buffer, "changed", G_CALLBACK(On_Changed));
		Connect_Focus(wid, tv);
		return TRUE;
	}

	if (wid->kind == W_GUI_WIDGET_TEXT) {
		GtkWidget *label = gtk_label_new(value);
		g_free(value);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0);
		gtk_label_set_yalign(GTK_LABEL(label), 0.0);
		gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
		return Place(wid, owner, label, x, y, w, h);
	}

	{
		GtkWidget *entry = gtk_entry_new();
		gtk_entry_set_text(GTK_ENTRY(entry), value);
		g_free(value);
		// `/secure` - fixed for the life of the field.
		if (wid->state & GUI_TEXT_SECURE) gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
		gtk_entry_set_width_chars(GTK_ENTRY(entry), 1);
		if (!Place(wid, owner, entry, x, y, w, h)) return FALSE;
		Connect(wid, entry, "changed", G_CALLBACK(On_Changed));
		// Enter is a `click`: the field was activated, not merely edited.
		Connect(wid, entry, "activate", G_CALLBACK(On_Activate));
		Connect_Focus(wid, entry);
		return TRUE;
	}
}

REBSER* Gui_Widget_Get_Text(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;

	switch (wid->kind) {
	case W_GUI_WIDGET_AREA: {
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(Inner_Of(wid)));
		GtkTextIter a, b;
		gchar *text;
		REBSER *out;
		gtk_text_buffer_get_bounds(buffer, &a, &b);
		text = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
		out = To_Rebol(text);
		g_free(text);
		return out; }
	case W_GUI_WIDGET_TEXT:
		return To_Rebol(gtk_label_get_text(GTK_LABEL(wid->handle)));
	case W_GUI_WIDGET_FIELD:
		return To_Rebol(gtk_entry_get_text(GTK_ENTRY(wid->handle)));
	case W_GUI_WIDGET_DROP_DOWN:
		return To_Rebol(gtk_entry_get_text(GTK_ENTRY(Combo_Entry(wid))));
	case W_GUI_WIDGET_DROP_LIST: {
		gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(wid->handle));
		REBSER *out = text ? To_Rebol(text) : NULL;
		g_free(text);
		return out; }
	case W_GUI_WIDGET_PANEL:
		return To_Rebol((const gchar*)g_object_get_data(G_OBJECT(wid->handle),
		                                                "rebol-gui-caption"));
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_TOGGLE:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO: {
		gchar *text = From_Mnemonic(gtk_button_get_label(GTK_BUTTON(wid->handle)));
		REBSER *out = To_Rebol(text);
		g_free(text);
		return out; }
	}
	return NULL;
}

REBOOL Gui_Widget_Set_Text(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	gchar *value;
	if (!wid || !wid->handle) return FALSE;
	value = Dup_Text(utf8, len);

	// A write from Rebol is not an edit by the user.
	Quiet++;
	switch (wid->kind) {
	case W_GUI_WIDGET_AREA:
		gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(Inner_Of(wid))), value, -1);
		break;
	case W_GUI_WIDGET_TEXT:
		gtk_label_set_text(GTK_LABEL(wid->handle), value);
		break;
	case W_GUI_WIDGET_FIELD:
		gtk_entry_set_text(GTK_ENTRY(wid->handle), value);
		break;
	case W_GUI_WIDGET_DROP_DOWN:
		gtk_entry_set_text(GTK_ENTRY(Combo_Entry(wid)), value);
		break;
	case W_GUI_WIDGET_PANEL:
		g_object_set_data_full(G_OBJECT(wid->handle), "rebol-gui-caption",
		                       g_strdup(value), g_free);
		gtk_widget_queue_draw(GTKW(wid));
		break;
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_TOGGLE:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO: {
		gchar *mn = To_Mnemonic(value);
		gtk_button_set_label(GTK_BUTTON(wid->handle), mn);
		gtk_button_set_use_underline(GTK_BUTTON(wid->handle), TRUE);
		g_free(mn);
		// The new label is a new widget, which has not had the font and
		// the colour yet.
		Apply_Text_Style(wid);
		break; }
	default:
		Quiet--;
		g_free(value);
		return FALSE;
	}
	Quiet--;
	g_free(value);
	return TRUE;
}


//== image and panel ==========================================================

REBOOL Gui_Create_Image(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *box;
	if (!wid || !owner || !owner->handle) return FALSE;
	box = New_Box(ROLE_IMAGE);
	REBOL_GUI_BOX(box)->wid = wid;
	return Place(wid, owner, box, x, y, w, h);
}

REBOOL Gui_Create_Panel(GUIWIDGET *wid, GUIWIN *owner,
                        REBINT x, REBINT y, REBINT w, REBINT h,
                        const REBYTE *text, REBCNT len)
{
	GtkWidget *box;
	if (!wid || !owner || !owner->handle) return FALSE;
	box = New_Box(ROLE_PANEL);
	REBOL_GUI_BOX(box)->wid = wid;
	if (text && len > 0)
		g_object_set_data_full(G_OBJECT(box), "rebol-gui-caption", Dup_Text(text, len), g_free);
	return Place(wid, owner, box, x, y, w, h);
}

void Gui_Panel_Border_Changed(GUIWIDGET *wid)
{
	if (wid && wid->handle) gtk_widget_queue_draw(GTKW(wid));
}

void Gui_Widget_Redraw(GUIWIDGET *wid)
{
	if (wid && wid->handle) gtk_widget_queue_draw(GTKW(wid));
}

void Gui_Widget_Invalidate(GUIWIDGET *wid)
{
	if (wid && wid->handle) gtk_widget_queue_draw(GTKW(wid));
}

void Gui_Window_Redraw(GUIWIN *win)
{
	if (win && win->handle) gtk_widget_queue_draw(GW(win)->window);
}

void Gui_Destroy_Widget(GUIWIDGET *wid)
{
	GtkWidget *w;
	// A NULL handle means the window already took the control with it.
	if (!wid || !wid->handle) return;
	w = GTKW(wid);
	Detach_Control(wid);
	wid->handle = NULL;
	gtk_widget_destroy(w);
}


//== line =====================================================================

REBOOL Gui_Create_Line(GUIWIDGET *wid, GUIWIN *owner,
                       REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *sep;
	if (!wid || !owner || !owner->handle) return FALSE;
	sep = gtk_separator_new(h > w ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
	// The rule is drawn along the middle of the box, not its top edge.
	if (h > w) gtk_widget_set_halign(sep, GTK_ALIGN_CENTER);
	else       gtk_widget_set_valign(sep, GTK_ALIGN_CENTER);
	return Place(wid, owner, sep, x, y, w, h);
}


//== slider and progress ======================================================

REBOOL Gui_Create_Range_Control(GUIWIDGET *wid, GUIWIN *owner,
                                REBINT x, REBINT y, REBINT w, REBINT h)
{
	if (!wid || !owner || !owner->handle) return FALSE;

	if (wid->kind == W_GUI_WIDGET_SLIDER) {
		// Taller than wide means upright, with 0.0 at the bottom - the
		// same rule as the other platforms.
		GtkWidget *scale = gtk_scale_new_with_range(
			h > w ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
		gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
		gtk_range_set_increments(GTK_RANGE(scale), 0.01, 0.1);
		if (h > w) {
			gtk_range_set_inverted(GTK_RANGE(scale), TRUE);
			gtk_widget_set_halign(scale, GTK_ALIGN_CENTER);
		}
		else gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
		gtk_range_set_value(GTK_RANGE(scale), 0.0);
		if (!Place(wid, owner, scale, x, y, w, h)) return FALSE;
		Connect(wid, scale, "value-changed", G_CALLBACK(On_Changed));
		return TRUE;
	}

	{
		GtkWidget *bar = gtk_progress_bar_new();
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), 0.0);
		gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
		return Place(wid, owner, bar, x, y, w, h);
	}
}

REBDEC Gui_Widget_Get_Value(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return 0.0;
	if (wid->kind == W_GUI_WIDGET_SLIDER) return (REBDEC)gtk_range_get_value(GTK_RANGE(wid->handle));
	return (REBDEC)gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(wid->handle));
}

void Gui_Widget_Set_Value(GUIWIDGET *wid, REBDEC value)
{
	if (!wid || !wid->handle) return;
	if (value < 0.0) value = 0.0;
	if (value > 1.0) value = 1.0;
	Quiet++;
	if (wid->kind == W_GUI_WIDGET_SLIDER) gtk_range_set_value(GTK_RANGE(wid->handle), value);
	else gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(wid->handle), value);
	Quiet--;
}


//== drop-list / drop-down ====================================================

// The toggle button inside a GtkComboBox - what takes the focus.
static void Find_Toggle(GtkWidget *w, gpointer out)
{
	if (!*(GtkWidget**)out && GTK_IS_TOGGLE_BUTTON(w)) *(GtkWidget**)out = w;
	else if (!*(GtkWidget**)out && GTK_IS_CONTAINER(w))
		gtk_container_forall(GTK_CONTAINER(w), Find_Toggle, out);
}

REBOOL Gui_Create_Drop_List(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *combo, *button = NULL;
	if (!wid || !owner || !owner->handle) return FALSE;
	combo = gtk_combo_box_text_new();
	if (!Place(wid, owner, combo, x, y, w, h)) return FALSE;
	gtk_container_forall(GTK_CONTAINER(combo), Find_Toggle, &button);
	if (button) {
		g_object_set_data(G_OBJECT(combo), KEY_INNER, button);
		Connect_Focus(wid, button);
	}
	Connect(wid, combo, "changed", G_CALLBACK(On_Changed));
	return TRUE;
}

REBOOL Gui_Create_Combo_Box(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *combo, *entry;
	GtkWidget *button = NULL;
	if (!wid || !owner || !owner->handle) return FALSE;
	combo = gtk_combo_box_text_new_with_entry();
	entry = gtk_bin_get_child(GTK_BIN(combo));
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 1);
	if (!Place(wid, owner, combo, x, y, w, h)) return FALSE;
	// One stop for Tab, the text, as a Win32 combo box has: the arrow is
	// for the mouse.
	gtk_container_forall(GTK_CONTAINER(combo), Find_Toggle, &button);
	if (button) gtk_widget_set_can_focus(button, FALSE);
	// A pick sets the entry's text, so both a pick and a keystroke land on
	// the entry's `changed` - one `change` each.
	Connect(wid, entry, "changed", G_CALLBACK(On_Changed));
	Connect_Focus(wid, entry);
	return TRUE;
}


//== text-list ================================================================

static void On_Selection(GtkTreeSelection *sel, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	GtkTreeModel *model;
	GtkTreeIter iter;
	if (Quiet || !wid || !wid->hob) return;
	if (wid->kind == W_GUI_WIDGET_TREE_VIEW) {
		gint node = -1;
		if (gtk_tree_selection_get_selected(sel, &model, &iter))
			gtk_tree_model_get(model, &iter, 1, &node, -1);
		Gui_Tree_Picked(wid, node);
		return;
	}
	if (wid->kind == W_GUI_WIDGET_LIST_VIEW) {
		// Filtered by the shared layer: only a row other than the last one
		// reported is a `change`.
		REBINT row = -1;
		if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
			GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
			row = gtk_tree_path_get_indices(path)[0];
			gtk_tree_path_free(path);
		}
		Gui_List_Picked(wid, row);
		return;
	}
	Queue_Widget(wid, EVT_CHANGE);
}

// With `scrollable?` off, the wheel does not move the list.
static gboolean On_List_Scroll(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	return (wid && (wid->state & GUI_LIST_FIXED)) ? TRUE : FALSE;
}

static GtkWidget *New_Table(GUIWIDGET *wid, GtkTreeModel *model, REBOOL both_bars)
{
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	GtkWidget *tree = gtk_tree_view_new_with_model(model);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
		both_bars ? GTK_POLICY_AUTOMATIC : GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)),
	                            GTK_SELECTION_SINGLE);
	gtk_tree_view_set_enable_search(GTK_TREE_VIEW(tree), FALSE);
	gtk_container_add(GTK_CONTAINER(scroll), tree);
	g_object_set_data(G_OBJECT(scroll), KEY_INNER, tree);
	g_object_set_data(G_OBJECT(tree), KEY_WIDGET, wid);
	return scroll;
}

static void Text_List_Cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                           GtkTreeModel *model, GtkTreeIter *iter, gpointer data);

REBOOL Gui_Create_Text_List(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkListStore *store;
	GtkWidget *scroll, *tree;
	GtkCellRenderer *cell;
	GtkTreeViewColumn *col;

	if (!wid || !owner || !owner->handle) return FALSE;
	store  = gtk_list_store_new(1, G_TYPE_STRING);
	scroll = New_Table(wid, GTK_TREE_MODEL(store), FALSE);
	g_object_unref(store);   // the tree view holds it
	tree = gtk_bin_get_child(GTK_BIN(scroll));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);

	cell = gtk_cell_renderer_text_new();
	col  = gtk_tree_view_column_new_with_attributes("", cell, "text", 0, NULL);
	gtk_tree_view_column_set_expand(col, TRUE);
	gtk_tree_view_column_set_cell_data_func(col, cell, Text_List_Cell, wid, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

	if (!Place(wid, owner, scroll, x, y, w, h)) return FALSE;
	Connect(wid, gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed",
	        G_CALLBACK(On_Selection));
	Connect(wid, scroll, "scroll-event", G_CALLBACK(On_List_Scroll));
	Connect_Focus(wid, tree);
	return TRUE;
}

void Gui_Widget_Set_Scrollable(GUIWIDGET *wid, REBOOL on)
{
	if (!wid || !wid->handle || wid->kind != W_GUI_WIDGET_TEXT_LIST) return;
	// EXTERNAL: no bar, but the list keeps its size and can still be
	// scrolled from code.
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(wid->handle), GTK_POLICY_NEVER,
	                               on ? GTK_POLICY_AUTOMATIC : GTK_POLICY_EXTERNAL);
}


//== list-view ================================================================
//
// The rows are not in GTK. RebolGuiRows is a tree model which only knows
// how many there are; each cell is asked of the shared layer as the tree
// view is about to show it (Gui_List_Cell), so only visible cells are
// ever formed.

typedef struct {
	GObject parent;
	gint    rows;
	gint    stamp;
} RebolGuiRows;

typedef struct {
	GObjectClass parent_class;
} RebolGuiRowsClass;

static GType Rows_Type(void);
#define REBOL_GUI_ROWS(o) ((RebolGuiRows*)(o))

static GtkTreeModelFlags Rows_Flags(GtkTreeModel *m)
{
	return GTK_TREE_MODEL_LIST_ONLY | GTK_TREE_MODEL_ITERS_PERSIST;
}

static gint  Rows_N_Columns(GtkTreeModel *m) { return 1; }
static GType Rows_Column_Type(GtkTreeModel *m, gint n) { return G_TYPE_INT; }

static gboolean Rows_Iter_Nth(GtkTreeModel *m, GtkTreeIter *iter, GtkTreeIter *parent, gint n)
{
	RebolGuiRows *r = REBOL_GUI_ROWS(m);
	if (parent || n < 0 || n >= r->rows) return FALSE;
	iter->stamp = r->stamp;
	iter->user_data = GINT_TO_POINTER(n);
	return TRUE;
}

static gboolean Rows_Get_Iter(GtkTreeModel *m, GtkTreeIter *iter, GtkTreePath *path)
{
	if (gtk_tree_path_get_depth(path) != 1) return FALSE;
	return Rows_Iter_Nth(m, iter, NULL, gtk_tree_path_get_indices(path)[0]);
}

static GtkTreePath *Rows_Get_Path(GtkTreeModel *m, GtkTreeIter *iter)
{
	return gtk_tree_path_new_from_indices(GPOINTER_TO_INT(iter->user_data), -1);
}

static void Rows_Get_Value(GtkTreeModel *m, GtkTreeIter *iter, gint column, GValue *value)
{
	g_value_init(value, G_TYPE_INT);
	g_value_set_int(value, GPOINTER_TO_INT(iter->user_data));
}

static gboolean Rows_Iter_Next(GtkTreeModel *m, GtkTreeIter *iter)
{
	gint n = GPOINTER_TO_INT(iter->user_data) + 1;
	if (n >= REBOL_GUI_ROWS(m)->rows) return FALSE;
	iter->user_data = GINT_TO_POINTER(n);
	return TRUE;
}

static gboolean Rows_Iter_Previous(GtkTreeModel *m, GtkTreeIter *iter)
{
	gint n = GPOINTER_TO_INT(iter->user_data) - 1;
	if (n < 0) return FALSE;
	iter->user_data = GINT_TO_POINTER(n);
	return TRUE;
}

static gboolean Rows_Iter_Children(GtkTreeModel *m, GtkTreeIter *iter, GtkTreeIter *parent)
{
	return Rows_Iter_Nth(m, iter, parent, 0);
}

static gboolean Rows_Iter_Has_Child(GtkTreeModel *m, GtkTreeIter *iter) { return FALSE; }

static gint Rows_Iter_N_Children(GtkTreeModel *m, GtkTreeIter *iter)
{
	return iter ? 0 : REBOL_GUI_ROWS(m)->rows;
}

static gboolean Rows_Iter_Parent(GtkTreeModel *m, GtkTreeIter *iter, GtkTreeIter *child)
{
	return FALSE;
}

static void Rows_Model_Init(gpointer g_iface, gpointer data)
{
	GtkTreeModelIface *iface = (GtkTreeModelIface*)g_iface;
	iface->get_flags       = Rows_Flags;
	iface->get_n_columns   = Rows_N_Columns;
	iface->get_column_type = Rows_Column_Type;
	iface->get_iter        = Rows_Get_Iter;
	iface->get_path        = Rows_Get_Path;
	iface->get_value       = Rows_Get_Value;
	iface->iter_next       = Rows_Iter_Next;
	iface->iter_previous   = Rows_Iter_Previous;
	iface->iter_children   = Rows_Iter_Children;
	iface->iter_has_child  = Rows_Iter_Has_Child;
	iface->iter_n_children = Rows_Iter_N_Children;
	iface->iter_nth_child  = Rows_Iter_Nth;
	iface->iter_parent     = Rows_Iter_Parent;
}

static GType Rows_Type(void)
{
	static GType type = 0;
	if (!type) {
		GTypeInfo info;
		GInterfaceInfo model;
		memset(&info, 0, sizeof(info));
		info.class_size    = sizeof(RebolGuiRowsClass);
		info.instance_size = sizeof(RebolGuiRows);
		type = g_type_register_static(G_TYPE_OBJECT, GUI_TYPE_NAME("Rows"), &info, 0);
		memset(&model, 0, sizeof(model));
		model.interface_init = Rows_Model_Init;
		g_type_add_interface_static(type, GTK_TYPE_TREE_MODEL, &model);
	}
	return type;
}

#define KEY_ROWS "rebol-gui-rows"

static RebolGuiRows *Rows_Of(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;
	return (RebolGuiRows*)g_object_get_data(G_OBJECT(wid->handle), KEY_ROWS);
}

// The row stripes and the text colour, as each cell is drawn: a colour
// of the widget's own only on rows that are not selected, which keep the
// theme's contrast.
static void Paint_Cell(GUIWIDGET *wid, GtkCellRenderer *cell, GtkTreeModel *model,
                       GtkTreeIter *iter, gint row)
{
	GtkWidget *tree = Inner_Of(wid);
	gboolean selected = FALSE;
	GdkRGBA rgba;

	if (tree) selected = gtk_tree_selection_iter_is_selected(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), iter);

	if (wid->kind == W_GUI_WIDGET_LIST_VIEW && GUI_LIST_STRIPED(wid)
	    && GUI_COLOR_HAS(wid->rows[row & 1]) && !selected) {
		Rgba_Of(wid->rows[row & 1], &rgba);
		g_object_set(cell, "cell-background-rgba", &rgba, NULL);
	} else {
		g_object_set(cell, "cell-background-set", FALSE, NULL);
	}
	if (GUI_COLOR_HAS(wid->color) && !selected) {
		Rgba_Of(wid->color, &rgba);
		g_object_set(cell, "foreground-rgba", &rgba, NULL);
	} else {
		g_object_set(cell, "foreground-set", FALSE, NULL);
	}
}

static void Text_List_Cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                           GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	GtkTreePath *path;
	if (!wid || !wid->handle) return;
	path = gtk_tree_model_get_path(model, iter);
	Paint_Cell(wid, cell, model, iter, gtk_tree_path_get_indices(path)[0]);
	gtk_tree_path_free(path);
}

static void List_View_Cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                           GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	gint row = GPOINTER_TO_INT(iter->user_data);
	REBCNT field = (REBCNT)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(col), KEY_FIELD));
	REBYTE *utf8 = NULL;
	REBCNT len = 0;
	gchar *text;

	if (!wid || !wid->handle || !wid->hob) return;
	if (Gui_List_Cell(wid, (REBCNT)row, field, &utf8, &len))
		text = g_strndup((const gchar*)utf8, len);
	else
		text = g_strdup("");
	g_object_set(cell, "text", text, NULL);
	g_free(text);
	Paint_Cell(wid, cell, model, iter, row);
}

static void On_Row_Activated(GtkTreeView *tree, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer data)
{
	Queue_Widget((GUIWIDGET*)data, EVT_CLICK);
}

static void On_Header_Clicked(GtkTreeViewColumn *col, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	REBINT x = 0, y = 0, w, h;
	REBCNT field = (REBCNT)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(col), KEY_FIELD));
	if (!wid || !wid->hob) return;
	Gui_Widget_Get_Box(wid, &x, &y, &w, &h);
	// The FIELD, hidden columns counted - what `sort/skip/compare` wants.
	Gui_Queue_Event(wid->hob, EVT_SORT, x, y, (REBINT)field + 1);
}

REBOOL Gui_Create_List_View(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	RebolGuiRows *rows;
	GtkWidget *scroll, *tree;

	if (!wid || !owner || !owner->handle) return FALSE;
	rows = (RebolGuiRows*)g_object_new(Rows_Type(), NULL);
	scroll = New_Table(wid, GTK_TREE_MODEL(rows), TRUE);
	tree = gtk_bin_get_child(GTK_BIN(scroll));
	g_object_set_data_full(G_OBJECT(scroll), KEY_ROWS, rows, g_object_unref);
	gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(tree), TRUE);
	gtk_tree_view_set_reorderable(GTK_TREE_VIEW(tree), FALSE);

	if (!Place(wid, owner, scroll, x, y, w, h)) return FALSE;
	Connect(wid, gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed",
	        G_CALLBACK(On_Selection));
	Connect(wid, tree, "row-activated", G_CALLBACK(On_Row_Activated));
	Connect_Focus(wid, tree);
	return TRUE;
}

REBOOL Gui_List_Add_Column(GUIWIDGET *wid, REBCNT field, const REBYTE *utf8, REBCNT len,
                           REBINT width, REBINT align)
{
	GtkWidget *tree = Inner_Of(wid);
	GtkCellRenderer *cell;
	GtkTreeViewColumn *col;
	gchar *title;
	gfloat xalign = (align == GUI_ALIGN_RIGHT) ? 1.0f : (align == GUI_ALIGN_CENTER) ? 0.5f : 0.0f;

	if (!tree) return FALSE;
	title = Dup_Text(utf8, len);
	cell = gtk_cell_renderer_text_new();
	g_object_set(cell, "xalign", xalign, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	col = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(col, title);
	gtk_tree_view_column_pack_start(col, cell, TRUE);
	gtk_tree_view_column_set_cell_data_func(col, cell, List_View_Cell, wid, NULL);
	gtk_tree_view_column_set_alignment(col, xalign);
	gtk_tree_view_column_set_resizable(col, TRUE);
	gtk_tree_view_column_set_clickable(col, TRUE);
	gtk_tree_view_column_set_reorderable(col, FALSE);
	gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
	g_object_set_data(G_OBJECT(col), KEY_FIELD, GUINT_TO_POINTER(field));

	if (width < 0) {
		// Fitting the title, and room for the sort arrow.
		PangoLayout *layout = gtk_widget_create_pango_layout(tree, title);
		gint tw = 0, th = 0;
		pango_layout_get_pixel_size(layout, &tw, &th);
		g_object_unref(layout);
		width = tw + 32;
	}
	gtk_tree_view_column_set_fixed_width(col, width > 0 ? width : 1);
	g_free(title);

	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
	Connect(wid, col, "clicked", G_CALLBACK(On_Header_Clicked));
	// A new header button, which has not had the font and colour yet.
	Apply_Text_Style(wid);
	return TRUE;
}

void Gui_List_Clear_Columns(GUIWIDGET *wid)
{
	GtkWidget *tree = Inner_Of(wid);
	GtkTreeViewColumn *col;
	if (!tree) return;
	while ((col = gtk_tree_view_get_column(GTK_TREE_VIEW(tree), 0)) != NULL)
		gtk_tree_view_remove_column(GTK_TREE_VIEW(tree), col);
}

// The last column expands into the room the others leave, and GTK keeps it
// so through resizes.
void Gui_List_Fill(GUIWIDGET *wid)
{
	GtkWidget *tree = Inner_Of(wid);
	gint n, count;
	if (!tree) return;
	count = (gint)gtk_tree_view_get_n_columns(GTK_TREE_VIEW(tree));
	for (n = 0; n < count; n++)
		gtk_tree_view_column_set_expand(gtk_tree_view_get_column(GTK_TREE_VIEW(tree), n),
			(n == count - 1 && (wid->state & GUI_LIST_FILL)) ? TRUE : FALSE);
}

REBCNT Gui_List_Column_Count(GUIWIDGET *wid)
{
	GtkWidget *tree = Inner_Of(wid);
	return tree ? (REBCNT)gtk_tree_view_get_n_columns(GTK_TREE_VIEW(tree)) : 0;
}

REBINT Gui_List_Column_Width(GUIWIDGET *wid, REBCNT field)
{
	GtkWidget *tree = Inner_Of(wid);
	gint n, count;
	if (!tree) return -1;
	count = (gint)gtk_tree_view_get_n_columns(GTK_TREE_VIEW(tree));
	for (n = 0; n < count; n++) {
		GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(tree), n);
		if ((REBCNT)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(col), KEY_FIELD)) == field) {
			gint w = gtk_tree_view_column_get_width(col);
			return w > 0 ? w : gtk_tree_view_column_get_fixed_width(col);
		}
	}
	return -1;   // hidden
}

void Gui_List_Reload(GUIWIDGET *wid, REBCNT rows)
{
	GtkWidget *tree = Inner_Of(wid);
	RebolGuiRows *model = Rows_Of(wid);
	if (!tree || !model) return;
	// A new count means new rows: the model is taken off and put back,
	// which is the cheapest way to tell a tree view everything changed.
	Quiet++;
	gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)));
	gtk_tree_view_set_model(GTK_TREE_VIEW(tree), NULL);
	model->rows = (gint)Gui_List_Rows(wid);
	model->stamp++;
	gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));
	Quiet--;
}

void Gui_List_Show_Header(GUIWIDGET *wid, REBOOL show)
{
	GtkWidget *tree = Inner_Of(wid);
	if (tree) gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), show ? TRUE : FALSE);
}

void Gui_List_Show_Sort(GUIWIDGET *wid)
{
	GtkWidget *tree = Inner_Of(wid);
	REBINT sorted = (wid->sort < 0) ? -wid->sort : wid->sort;   // a field, 1-based
	gint n, count;
	if (!tree) return;
	count = (gint)gtk_tree_view_get_n_columns(GTK_TREE_VIEW(tree));
	for (n = 0; n < count; n++) {
		GtkTreeViewColumn *col = gtk_tree_view_get_column(GTK_TREE_VIEW(tree), n);
		REBCNT field = (REBCNT)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(col), KEY_FIELD));
		gboolean on = (REBINT)field + 1 == sorted;
		gtk_tree_view_column_set_sort_indicator(col, on);
		if (on) gtk_tree_view_column_set_sort_order(col,
			wid->sort > 0 ? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
	}
}


//== tree-view ================================================================
//
// A GtkTreeStore of two columns: the label, and the node's index in the
// shared layer's table (see gui.h). The store's iterators persist, so
// each node's is kept, by index, to find it again.
//
// Which branches are open is kept here too, per node. GTK forgets that a
// branch was open once the one holding it closes - its rows are gone -
// and cannot open a branch it is not showing; a Windows tree remembers,
// and opens one whenever. So each node's wish is recorded, and applied
// as the branch holding it opens.

#define KEY_ITERS "rebol-gui-iters"   // GArray of GtkTreeIter, by node
#define KEY_OPEN  "rebol-gui-open"    // GArray of guint8, by node

static GArray *Tree_Iters(GUIWIDGET *wid)
{
	return wid && wid->handle
		? (GArray*)g_object_get_data(G_OBJECT(wid->handle), KEY_ITERS) : NULL;
}

static GArray *Tree_Open(GUIWIDGET *wid)
{
	return wid && wid->handle
		? (GArray*)g_object_get_data(G_OBJECT(wid->handle), KEY_OPEN) : NULL;
}

static GtkTreeStore *Tree_Store(GUIWIDGET *wid)
{
	GtkWidget *tree = Inner_Of(wid);
	return tree ? GTK_TREE_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(tree))) : NULL;
}

static GtkTreePath *Tree_Path_Of(GUIWIDGET *wid, REBCNT n)
{
	GArray *iters = Tree_Iters(wid);
	GtkTreeStore *store = Tree_Store(wid);
	if (!iters || !store || n >= iters->len) return NULL;
	return gtk_tree_model_get_path(GTK_TREE_MODEL(store),
	                               &g_array_index(iters, GtkTreeIter, n));
}

static REBCNT Tree_Node_At(GtkTreeModel *model, GtkTreeIter *iter)
{
	gint node = -1;
	gtk_tree_model_get(model, iter, 1, &node, -1);
	return (REBCNT)node;
}

// A branch opened, by the user or from code: every branch inside it that
// was open before, opens again.
static void On_Row_Expanded(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path,
                            gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	GArray *open = Tree_Open(wid);
	GtkTreeModel *model = gtk_tree_view_get_model(view);
	GtkTreeIter child;
	REBCNT n = Tree_Node_At(model, iter);

	if (!open) return;
	if (n < open->len) g_array_index(open, guint8, n) = 1;
	if (!gtk_tree_model_iter_children(model, &child, iter)) return;
	do {
		REBCNT c = Tree_Node_At(model, &child);
		if (c < open->len && g_array_index(open, guint8, c)) {
			GtkTreePath *p = gtk_tree_model_get_path(model, &child);
			gtk_tree_view_expand_row(view, p, FALSE);
			gtk_tree_path_free(p);
		}
	} while (gtk_tree_model_iter_next(model, &child));
}

static void On_Row_Collapsed(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path,
                             gpointer data)
{
	GArray *open = Tree_Open((GUIWIDGET*)data);
	REBCNT n = Tree_Node_At(gtk_tree_view_get_model(view), iter);
	if (open && n < open->len) g_array_index(open, guint8, n) = 0;
}

/***********************************************************************
**  Left and Right open and close branches, as in a Windows tree and an
**  NSOutlineView: Right opens a closed branch, or goes to the first node
**  in an open one; Left closes an open branch, or goes to the parent.
**  GTK's own keys for that (+, - and Shift with the arrows) still work.
***********************************************************************/
static gboolean On_Tree_Key(GtkWidget *w, GdkEventKey *key, gpointer data)
{
	GtkTreeView  *view = GTK_TREE_VIEW(w);
	GtkTreeModel *model = gtk_tree_view_get_model(view);
	GtkTreePath  *path = NULL;
	GtkTreeIter   iter;
	gboolean      right, done = FALSE;

	if (key->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)) return FALSE;
	switch (key->keyval) {
	case GDK_KEY_Right: case GDK_KEY_KP_Right: right = TRUE;  break;
	case GDK_KEY_Left:  case GDK_KEY_KP_Left:  right = FALSE; break;
	default: return FALSE;
	}
	gtk_tree_view_get_cursor(view, &path, NULL);
	if (!path || !model || !gtk_tree_model_get_iter(model, &iter, path)) {
		if (path) gtk_tree_path_free(path);
		return FALSE;
	}

	if (right) {
		if (gtk_tree_model_iter_has_child(model, &iter)) {
			if (!gtk_tree_view_row_expanded(view, path))
				gtk_tree_view_expand_row(view, path, FALSE);
			else {
				gtk_tree_path_down(path);
				gtk_tree_view_set_cursor(view, path, NULL, FALSE);
			}
		}
		done = TRUE;
	} else {
		if (gtk_tree_view_row_expanded(view, path))
			gtk_tree_view_collapse_row(view, path);
		else if (gtk_tree_path_get_depth(path) > 1) {
			gtk_tree_path_up(path);
			gtk_tree_view_set_cursor(view, path, NULL, FALSE);
		}
		done = TRUE;
	}
	gtk_tree_path_free(path);
	return done;
}

static void Tree_Cell(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	if (!wid || !wid->handle) return;
	Paint_Cell(wid, cell, model, iter, 0);
}

REBOOL Gui_Create_Tree_View(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkTreeStore *store;
	GtkWidget *scroll, *tree;
	GtkCellRenderer *cell;
	GtkTreeViewColumn *col;

	if (!wid || !owner || !owner->handle) return FALSE;
	store  = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_INT);
	scroll = New_Table(wid, GTK_TREE_MODEL(store), TRUE);
	g_object_unref(store);   // the tree view holds it
	tree = gtk_bin_get_child(GTK_BIN(scroll));
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), FALSE);
	// Lines joining a node to its parent and siblings, as a Windows tree
	// draws them.
	gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tree), TRUE);

	cell = gtk_cell_renderer_text_new();
	col  = gtk_tree_view_column_new_with_attributes("", cell, "text", 0, NULL);
	gtk_tree_view_column_set_cell_data_func(col, cell, Tree_Cell, wid, NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

	g_object_set_data_full(G_OBJECT(scroll), KEY_ITERS,
		g_array_new(FALSE, TRUE, sizeof(GtkTreeIter)), (GDestroyNotify)g_array_unref);
	g_object_set_data_full(G_OBJECT(scroll), KEY_OPEN,
		g_array_new(FALSE, TRUE, sizeof(guint8)), (GDestroyNotify)g_array_unref);

	if (!Place(wid, owner, scroll, x, y, w, h)) return FALSE;
	Connect(wid, gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), "changed",
	        G_CALLBACK(On_Selection));
	Connect(wid, tree, "row-activated", G_CALLBACK(On_Row_Activated));
	Connect(wid, tree, "row-expanded", G_CALLBACK(On_Row_Expanded));
	Connect(wid, tree, "row-collapsed", G_CALLBACK(On_Row_Collapsed));
	Connect(wid, tree, "key-press-event", G_CALLBACK(On_Tree_Key));
	Connect_Focus(wid, tree);
	return TRUE;
}

void Gui_Tree_Clear(GUIWIDGET *wid)
{
	GtkTreeStore *store = Tree_Store(wid);
	GArray *iters = Tree_Iters(wid), *open = Tree_Open(wid);
	if (!store) return;
	Quiet++;
	gtk_tree_store_clear(store);
	Quiet--;
	if (iters) g_array_set_size(iters, 0);
	if (open)  g_array_set_size(open, 0);
}

void* Gui_Tree_Add_Node(GUIWIDGET *wid, REBCNT n)
{
	GUITREE *tree = GUI_TREE_OF(wid);
	GtkTreeStore *store = Tree_Store(wid);
	GArray *iters = Tree_Iters(wid), *open = Tree_Open(wid);
	GUITREENODE *node;
	GtkTreeIter iter;

	if (!tree || !store || !iters || !open || n >= tree->count) return NULL;
	node = &tree->nodes[n];
	if (iters->len <= n) g_array_set_size(iters, n + 1);
	if (open->len <= n)  g_array_set_size(open, n + 1);

	gtk_tree_store_append(store, &iter, node->parent >= 0
		? &g_array_index(iters, GtkTreeIter, node->parent) : NULL);
	gtk_tree_store_set(store, &iter, 0, (const gchar*)node->label, 1, (gint)n, -1);
	g_array_index(iters, GtkTreeIter, n) = iter;
	g_array_index(open, guint8, n) = 0;
	// Anything not NULL: the iterator itself is kept here, by index.
	return GUINT_TO_POINTER(n + 1);
}

void Gui_Tree_End(GUIWIDGET *wid)
{
	(void)wid;
}

void Gui_Tree_Select(GUIWIDGET *wid, REBINT n)
{
	GtkWidget *tree = Inner_Of(wid);
	GtkTreeSelection *sel;
	GtkTreePath *path;

	if (!tree) return;
	sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
	Quiet++;
	if (n < 0 || !(path = Tree_Path_Of(wid, (REBCNT)n))) {
		gtk_tree_selection_unselect_all(sel);
	} else {
		gtk_tree_selection_select_path(sel, path);
		gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, FALSE, 0, 0);
		gtk_tree_path_free(path);
	}
	Quiet--;
}

REBINT Gui_Tree_Selected(GUIWIDGET *wid)
{
	GtkWidget *tree = Inner_Of(wid);
	GtkTreeModel *model;
	GtkTreeIter iter;
	if (!tree || !gtk_tree_selection_get_selected(
		gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &model, &iter)) return -1;
	return (REBINT)Tree_Node_At(model, &iter);
}

void Gui_Tree_Expand(GUIWIDGET *wid, REBCNT n, REBOOL on)
{
	GtkWidget *tree = Inner_Of(wid);
	GArray *open = Tree_Open(wid);
	GtkTreePath *path;

	if (!tree || !open || n >= open->len) return;
	g_array_index(open, guint8, n) = on ? 1 : 0;
	path = Tree_Path_Of(wid, n);
	if (!path) return;
	// Shown or not; one inside a closed branch opens with it - see
	// On_Row_Expanded.
	// Quiet: closing a branch drops a selection inside it, which is the
	// script's doing, not the user's.
	Quiet++;
	if (on) gtk_tree_view_expand_row(GTK_TREE_VIEW(tree), path, FALSE);
	else    gtk_tree_view_collapse_row(GTK_TREE_VIEW(tree), path);
	Quiet--;
	// Collapsing a row takes its own wish with it; this one was asked for.
	g_array_index(open, guint8, n) = on ? 1 : 0;
	gtk_tree_path_free(path);
}

REBOOL Gui_Tree_Is_Expanded(GUIWIDGET *wid, REBCNT n)
{
	GArray *open = Tree_Open(wid);
	return (open && n < open->len && g_array_index(open, guint8, n)) ? TRUE : FALSE;
}


//-- items, for every kind which has them -------------------------------------

REBCNT Gui_Widget_Count_Items(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return 0;
	switch (wid->kind) {
	case W_GUI_WIDGET_TAB_PANEL:
		return (REBCNT)gtk_notebook_get_n_pages(GTK_NOTEBOOK(wid->handle));
	case W_GUI_WIDGET_LIST_VIEW:
		return Gui_List_Rows(wid);
	case W_GUI_WIDGET_TEXT_LIST:
		return (REBCNT)gtk_tree_model_iter_n_children(
			gtk_tree_view_get_model(GTK_TREE_VIEW(Inner_Of(wid))), NULL);
	case W_GUI_WIDGET_DROP_LIST:
	case W_GUI_WIDGET_DROP_DOWN:
		return (REBCNT)gtk_tree_model_iter_n_children(
			gtk_combo_box_get_model(GTK_COMBO_BOX(wid->handle)), NULL);
	}
	return 0;
}

static GtkTreeModel *Item_Model(GUIWIDGET *wid)
{
	if (wid->kind == W_GUI_WIDGET_TEXT_LIST)
		return gtk_tree_view_get_model(GTK_TREE_VIEW(Inner_Of(wid)));
	if (wid->kind == W_GUI_WIDGET_DROP_LIST || wid->kind == W_GUI_WIDGET_DROP_DOWN)
		return gtk_combo_box_get_model(GTK_COMBO_BOX(wid->handle));
	return NULL;
}

REBSER* Gui_Widget_Get_Item(GUIWIDGET *wid, REBCNT n)
{
	GtkTreeModel *model;
	GtkTreeIter iter;
	gchar *text = NULL;
	REBSER *out;

	if (!wid || !wid->handle) return NULL;
	if (wid->kind == W_GUI_WIDGET_TAB_PANEL) {
		GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(wid->handle), (gint)n);
		if (!page) return NULL;
		return To_Rebol(gtk_notebook_get_tab_label_text(GTK_NOTEBOOK(wid->handle), page));
	}
	model = Item_Model(wid);
	if (!model || !gtk_tree_model_iter_nth_child(model, &iter, NULL, (gint)n)) return NULL;
	gtk_tree_model_get(model, &iter, 0, &text, -1);
	out = To_Rebol(text ? text : "");
	g_free(text);
	return out;
}

static GtkWidget *Tab_Label(GUIWIDGET *tabs, const gchar *text);

REBOOL Gui_Widget_Add_Item(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	gchar *text;
	if (!wid || !wid->handle) return FALSE;
	text = Dup_Text(utf8, len);

	Quiet++;
	switch (wid->kind) {
	case W_GUI_WIDGET_TAB_PANEL: {
		// The page is made now, empty, and becomes a widget when the shared
		// layer asks for it - see Gui_Create_Tab_Page.
		GtkWidget *page = New_Box(ROLE_PANEL);
		gtk_widget_show(page);
		gtk_notebook_append_page(GTK_NOTEBOOK(wid->handle), page, Tab_Label(wid, text));
		gtk_container_child_set(GTK_CONTAINER(wid->handle), page, "tab-expand", FALSE, NULL);
		break; }
	case W_GUI_WIDGET_TEXT_LIST: {
		GtkListStore *store = GTK_LIST_STORE(Item_Model(wid));
		GtkTreeIter iter;
		gtk_list_store_append(store, &iter);
		gtk_list_store_set(store, &iter, 0, text, -1);
		break; }
	case W_GUI_WIDGET_DROP_LIST:
	case W_GUI_WIDGET_DROP_DOWN:
		gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(wid->handle), text);
		break;
	default:
		Quiet--;
		g_free(text);
		return FALSE;
	}
	Quiet--;
	g_free(text);
	return TRUE;
}

void Gui_Widget_Clear_Items(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return;
	Quiet++;
	switch (wid->kind) {
	case W_GUI_WIDGET_TEXT_LIST:
		gtk_list_store_clear(GTK_LIST_STORE(Item_Model(wid)));
		break;
	case W_GUI_WIDGET_DROP_LIST:
		gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(wid->handle));
		break;
	case W_GUI_WIDGET_DROP_DOWN: {
		// Removing the items leaves the typed text, as on the other
		// platforms.
		gtk_list_store_clear(GTK_LIST_STORE(Item_Model(wid)));
		break; }
	}
	Quiet--;
}

static void Show_Row(GUIWIDGET *wid, REBINT n);

REBINT Gui_Widget_Get_Index(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return -1;
	switch (wid->kind) {
	case W_GUI_WIDGET_TAB_PANEL:
		return (REBINT)gtk_notebook_get_current_page(GTK_NOTEBOOK(wid->handle));
	case W_GUI_WIDGET_TEXT_LIST:
	case W_GUI_WIDGET_LIST_VIEW: {
		GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(Inner_Of(wid)));
		GtkTreeModel *model;
		GtkTreeIter iter;
		GtkTreePath *path;
		REBINT n;
		if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return -1;
		path = gtk_tree_model_get_path(model, &iter);
		n = gtk_tree_path_get_indices(path)[0];
		gtk_tree_path_free(path);
		return n; }
	case W_GUI_WIDGET_DROP_LIST:
	case W_GUI_WIDGET_DROP_DOWN:
		return (REBINT)gtk_combo_box_get_active(GTK_COMBO_BOX(wid->handle));
	}
	return -1;
}

void Gui_Widget_Set_Index(GUIWIDGET *wid, REBINT n)
{
	if (!wid || !wid->handle) return;
	Quiet++;
	switch (wid->kind) {
	case W_GUI_WIDGET_TAB_PANEL:
		// Always one page: out of range changes nothing.
		if (n >= 0 && n < gtk_notebook_get_n_pages(GTK_NOTEBOOK(wid->handle)))
			gtk_notebook_set_current_page(GTK_NOTEBOOK(wid->handle), n);
		break;
	case W_GUI_WIDGET_TEXT_LIST:
	case W_GUI_WIDGET_LIST_VIEW: {
		GtkTreeView *tree = GTK_TREE_VIEW(Inner_Of(wid));
		GtkTreeSelection *sel = gtk_tree_view_get_selection(tree);
		GtkTreeModel *model = gtk_tree_view_get_model(tree);
		if (!model || n < 0 || n >= gtk_tree_model_iter_n_children(model, NULL)) {
			gtk_tree_selection_unselect_all(sel);
		} else {
			GtkTreePath *path = gtk_tree_path_new_from_indices(n, -1);
			gtk_tree_selection_select_path(sel, path);
			gtk_tree_path_free(path);
			Show_Row(wid, n);
		}
		break; }
	case W_GUI_WIDGET_DROP_LIST:
		gtk_combo_box_set_active(GTK_COMBO_BOX(wid->handle),
			(n >= 0 && n < (REBINT)Gui_Widget_Count_Items(wid)) ? n : -1);
		break;
	case W_GUI_WIDGET_DROP_DOWN:
		if (n >= 0 && n < (REBINT)Gui_Widget_Count_Items(wid))
			gtk_combo_box_set_active(GTK_COMBO_BOX(wid->handle), n);
		else {
			gtk_combo_box_set_active(GTK_COMBO_BOX(wid->handle), -1);
			gtk_entry_set_text(GTK_ENTRY(Combo_Entry(wid)), "");
		}
		break;
	}
	Quiet--;
}


//== tab-panel ================================================================

static GtkWidget *Tab_Label(GUIWIDGET *tabs, const gchar *text)
{
	GtkWidget *label = gtk_label_new(text);
	gtk_widget_show(label);
	return label;
}

static void On_Switch_Page(GtkNotebook *nb, GtkWidget *page, guint n, gpointer data)
{
	if (Quiet) return;
	Queue_Widget((GUIWIDGET*)data, EVT_CHANGE);
}

REBOOL Gui_Create_Tab_Panel(GUIWIDGET *wid, GUIWIN *owner,
                            REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *nb;
	if (!wid || !owner || !owner->handle) return FALSE;
	nb = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);
	if (!Place(wid, owner, nb, x, y, w, h)) return FALSE;
	Connect(wid, nb, "switch-page", G_CALLBACK(On_Switch_Page));
	return TRUE;
}

// The page already exists - Gui_Widget_Add_Item made it with its tab - and
// only becomes the widget's here.
REBOOL Gui_Create_Tab_Page(GUIWIDGET *page, GUIWIDGET *tabs, GUIWIN *owner)
{
	GtkWidget *box;
	if (!page || !tabs || !tabs->handle) return FALSE;
	box = gtk_notebook_get_nth_page(GTK_NOTEBOOK(tabs->handle), (gint)page->group - 1);
	if (!box || !IS_BOX(box)) return FALSE;
	REBOL_GUI_BOX(box)->wid = page;
	g_object_set_data(G_OBJECT(box), KEY_WIDGET, page);
	gtk_widget_add_events(box, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
	                         | GDK_BUTTON_RELEASE_MASK);
	page->handle = (void*)box;
	return TRUE;
}


//== date-field ===============================================================
//
// GTK 3 has no date picker, so this is an entry showing the date in the
// user's short format, and a calendar in a popover behind the entry's
// icon. The value is kept here, not parsed out of the text at every read;
// what the user types is taken when they press Enter or leave the field.

typedef struct {
	GUIDATE    value;
	GtkWidget *popover;
	GtkWidget *calendar;
} DATEFIELD;

static DATEFIELD *Date_Of(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;
	return (DATEFIELD*)g_object_get_data(G_OBJECT(wid->handle), KEY_DATE);
}

// The user's short date - with the century, as on Windows; a locale whose
// short date has a two-digit year gets ISO order instead.
static gchar *Format_Date(const GUIDATE *d, REBOOL with_time)
{
	REBI64 s = with_time ? d->ns / 1000000000 : 0;
	GDateTime *dt = g_date_time_new_local(d->year, d->month, d->day,
		(gint)(s / 3600), (gint)((s / 60) % 60), (gdouble)(s % 60));
	gchar *date, *year, *out;
	if (!dt) return g_strdup("");
	date = g_date_time_format(dt, "%x");
	year = g_strdup_printf("%d", d->year);
	if (!date || !strstr(date, year)) {
		g_free(date);
		date = g_date_time_format(dt, "%Y-%m-%d");
	}
	if (with_time) {
		gchar *time = g_date_time_format(dt, "%H:%M");
		out = g_strdup_printf("%s %s", date, time);
		g_free(time);
		g_free(date);
	} else out = date;
	g_free(year);
	g_date_time_unref(dt);
	return out;
}

static void Show_Date(GUIWIDGET *wid)
{
	DATEFIELD *df = Date_Of(wid);
	gchar *text;
	if (!df) return;
	text = Format_Date(&df->value, (wid->state & GUI_DATE_TIME) ? TRUE : FALSE);
	Quiet++;
	gtk_entry_set_text(GTK_ENTRY(wid->handle), text);
	Quiet--;
	g_free(text);
}

// What the user typed, taken as the new value if it reads as a date (and a
// time); otherwise the text goes back to the value it had.
static void Commit_Date(GUIWIDGET *wid)
{
	DATEFIELD *df = Date_Of(wid);
	gchar *text, *time_part = NULL;
	GDate *date;
	GUIDATE nd;

	if (!df) return;
	text = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(wid->handle))));
	nd = df->value;

	if (wid->state & GUI_DATE_TIME) {
		gchar *space = strrchr(text, ' ');
		if (space && strchr(space, ':')) { *space = 0; time_part = space + 1; }
	}
	date = g_date_new();
	g_date_set_parse(date, text);
	if (g_date_valid(date)) {
		nd.year  = g_date_get_year(date);
		nd.month = g_date_get_month(date);
		nd.day   = g_date_get_day(date);
		if (time_part) {
			int hh = 0, mm = 0, ss = 0;
			if (sscanf(time_part, "%d:%d:%d", &hh, &mm, &ss) >= 2
			    && hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60)
				nd.ns = ((REBI64)hh * 3600 + mm * 60 + ss) * 1000000000;
		}
		if (nd.year != df->value.year || nd.month != df->value.month
		    || nd.day != df->value.day || nd.ns != df->value.ns) {
			df->value = nd;
			Queue_Widget(wid, EVT_CHANGE);
		}
	}
	g_date_free(date);
	g_free(text);
	Show_Date(wid);
}

static void On_Date_Activate(GtkEntry *entry, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	Commit_Date(wid);
	Queue_Widget(wid, EVT_CLICK);
}

static gboolean On_Date_Focus_Out(GtkWidget *w, GdkEvent *ev, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	DATEFIELD *df = Date_Of(wid);
	// Going to its own calendar is not leaving the field.
	if (df && df->popover && gtk_widget_get_visible(df->popover)) return FALSE;
	Commit_Date(wid);
	Queue_Widget(wid, EVT_UNFOCUS);
	return FALSE;
}

static void On_Day_Selected(GtkCalendar *cal, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	DATEFIELD *df = Date_Of(wid);
	guint y, m, d;
	if (Quiet || !df) return;
	gtk_calendar_get_date(cal, &y, &m, &d);
	if ((REBINT)y == df->value.year && (REBINT)m + 1 == df->value.month
	    && (REBINT)d == df->value.day) return;
	df->value.year = (REBINT)y;
	df->value.month = (REBINT)m + 1;
	df->value.day = (REBINT)d;
	Show_Date(wid);
	Queue_Widget(wid, EVT_CHANGE);
}

static void On_Day_Picked(GtkCalendar *cal, gpointer data)
{
	DATEFIELD *df = Date_Of((GUIWIDGET*)data);
	if (df && df->popover) gtk_popover_popdown(GTK_POPOVER(df->popover));
}

static void On_Date_Icon(GtkEntry *entry, GtkEntryIconPosition pos, GdkEvent *ev, gpointer data)
{
	GUIWIDGET *wid = (GUIWIDGET*)data;
	DATEFIELD *df = Date_Of(wid);
	GdkRectangle r;
	if (!df || !gtk_widget_is_sensitive(GTK_WIDGET(entry))) return;
	Commit_Date(wid);
	Quiet++;
	gtk_calendar_select_month(GTK_CALENDAR(df->calendar),
	                          (guint)df->value.month - 1, (guint)df->value.year);
	gtk_calendar_select_day(GTK_CALENDAR(df->calendar), (guint)df->value.day);
	Quiet--;
	gtk_entry_get_icon_area(entry, pos, &r);
	gtk_popover_set_pointing_to(GTK_POPOVER(df->popover), &r);
	gtk_popover_popup(GTK_POPOVER(df->popover));
}

static void Free_Date(gpointer p)
{
	DATEFIELD *df = (DATEFIELD*)p;
	if (df->popover) gtk_widget_destroy(df->popover);
	g_free(df);
}

REBOOL Gui_Create_Date_Field(GUIWIDGET *wid, GUIWIN *owner,
                             REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *entry;
	DATEFIELD *df;
	GDateTime *now;
	GUIDATE wide;
	gchar *sample;

	if (!wid || !owner || !owner->handle) return FALSE;

	entry = gtk_entry_new();
	df = g_new0(DATEFIELD, 1);
	g_object_set_data_full(G_OBJECT(entry), KEY_DATE, df, Free_Date);

	// Today, at the time it is now - the same start as the other platforms.
	now = g_date_time_new_now_local();
	df->value.year  = g_date_time_get_year(now);
	df->value.month = g_date_time_get_month(now);
	df->value.day   = g_date_time_get_day_of_month(now);
	df->value.ns    = (wid->state & GUI_DATE_TIME)
		? ((REBI64)g_date_time_get_hour(now) * 3600 + g_date_time_get_minute(now) * 60
		   + g_date_time_get_second(now)) * 1000000000 : 0;
	g_date_time_unref(now);

	// As wide as the widest date it can show, so a later one is not clipped.
	wide.year = 2000; wide.month = 12; wide.day = 28; wide.ns = (REBI64)75538 * 1000000000;
	sample = Format_Date(&wide, (wid->state & GUI_DATE_TIME) ? TRUE : FALSE);
	gtk_entry_set_width_chars(GTK_ENTRY(entry), (gint)g_utf8_strlen(sample, -1) + 1);
	g_free(sample);

	gtk_entry_set_icon_from_icon_name(GTK_ENTRY(entry), GTK_ENTRY_ICON_SECONDARY,
	                                  "x-office-calendar-symbolic");
	df->popover  = gtk_popover_new(entry);
	df->calendar = gtk_calendar_new();
	gtk_container_add(GTK_CONTAINER(df->popover), df->calendar);
	gtk_widget_show(df->calendar);

	if (!Place(wid, owner, entry, x, y, w, h)) return FALSE;
	gtk_widget_set_valign(entry, GTK_ALIGN_CENTER);
	Show_Date(wid);

	Connect(wid, entry, "activate", G_CALLBACK(On_Date_Activate));
	Connect(wid, entry, "icon-press", G_CALLBACK(On_Date_Icon));
	Connect(wid, entry, "focus-in-event", G_CALLBACK(On_Focus_In));
	Connect(wid, entry, "focus-out-event", G_CALLBACK(On_Date_Focus_Out));
	Connect(wid, df->calendar, "day-selected", G_CALLBACK(On_Day_Selected));
	Connect(wid, df->calendar, "day-selected-double-click", G_CALLBACK(On_Day_Picked));
	return TRUE;
}

REBOOL Gui_Widget_Get_Date(GUIWIDGET *wid, GUIDATE *out)
{
	DATEFIELD *df = Date_Of(wid);
	if (!df || !out) return FALSE;
	*out = df->value;
	if (!(wid->state & GUI_DATE_TIME)) out->ns = 0;
	return TRUE;
}

void Gui_Widget_Set_Date(GUIWIDGET *wid, const GUIDATE *in)
{
	DATEFIELD *df = Date_Of(wid);
	if (!df || !in) return;
	// An impossible date is refused, as on the other platforms.
	if (in->month < 1 || in->month > 12 || in->day < 1 || in->year < 1
	    || !g_date_valid_dmy((GDateDay)in->day, (GDateMonth)in->month, (GDateYear)in->year))
		return;
	df->value.year  = in->year;
	df->value.month = in->month;
	df->value.day   = in->day;
	if (wid->state & GUI_DATE_TIME) df->value.ns = in->ns;
	Show_Date(wid);
}


//== geometry =================================================================

static void Layout_Window(GUIWIN *win, REBOOL measure);

// A tab page is placed by its notebook, at the next layout; one which has
// not had it yet is laid out first, so its place can be answered now.
static void Place_Pages(GUIWIDGET *wid)
{
	GUIWIDGET *w;
	for (w = wid; w; w = (GUIWIDGET*)w->parent) {
		GtkWidget *g = GTKW(w);
		GtkWidget *parent = g ? gtk_widget_get_parent(g) : NULL;
		if (parent && GTK_IS_NOTEBOOK(parent)) {
			GtkAllocation pa, na;
			gtk_widget_get_allocation(g, &pa);
			gtk_widget_get_allocation(parent, &na);
			if (pa.y <= na.y) { Layout_Window(wid->owner, FALSE); return; }
		}
	}
}

REBOOL Gui_Widget_Get_Box(GUIWIDGET *wid, REBINT *x, REBINT *y, REBINT *w, REBINT *h)
{
	GtkWidget *widget, *parent;
	BOXCHILD *c;

	if (!wid || !wid->handle) return FALSE;
	widget = GTKW(wid);
	parent = gtk_widget_get_parent(widget);
	if (parent && IS_BOX(parent) && (c = Box_Find(REBOL_GUI_BOX(parent), widget))) {
		*x = c->x; *y = c->y; *w = c->w; *h = c->h;
		return TRUE;
	}
	// A tab page: placed by its notebook.
	if (parent && GTK_IS_NOTEBOOK(parent)) {
		GtkAllocation pa, na;
		Place_Pages(wid);
		gtk_widget_get_allocation(widget, &pa);
		gtk_widget_get_allocation(parent, &na);
		*x = pa.x - na.x; *y = pa.y - na.y;
		*w = pa.width; *h = pa.height;
		return TRUE;
	}
	return FALSE;
}

REBOOL Gui_Widget_Set_Box(GUIWIDGET *wid, REBINT x, REBINT y, REBINT w, REBINT h)
{
	GtkWidget *widget, *parent;
	if (!wid || !wid->handle) return FALSE;
	widget = GTKW(wid);
	parent = gtk_widget_get_parent(widget);
	if (parent && IS_BOX(parent)) Box_Move(parent, widget, x, y, w, h);
	// A tab page's box is its notebook's to decide.
	return TRUE;
}

REBOOL Gui_Widget_Get_At(GUIWIDGET *wid, REBINT *x, REBINT *y)
{
	if (!wid || !wid->handle || !wid->owner || !wid->owner->handle) return FALSE;
	Place_Pages(wid);
	return Widget_Origin(wid, x, y);
}


//== state ====================================================================

REBOOL Gui_Widget_Get_Enabled(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	return gtk_widget_get_sensitive(GTKW(wid)) ? TRUE : FALSE;
}

// GTK keeps enabled and editable apart, so read-only needs no combining
// with the enabled state here.
REBOOL Gui_Widget_Set_Enabled(GUIWIDGET *wid, REBOOL enabled)
{
	if (!wid || !wid->handle) return FALSE;
	gtk_widget_set_sensitive(GTKW(wid), enabled ? TRUE : FALSE);
	return TRUE;
}

REBOOL Gui_Widget_Set_Read_Only(GUIWIDGET *wid, REBOOL on)
{
	if (!wid || !wid->handle) return FALSE;
	if (wid->kind == W_GUI_WIDGET_AREA)
		gtk_text_view_set_editable(GTK_TEXT_VIEW(Inner_Of(wid)), on ? FALSE : TRUE);
	else if (wid->kind == W_GUI_WIDGET_FIELD)
		gtk_editable_set_editable(GTK_EDITABLE(wid->handle), on ? FALSE : TRUE);
	else return FALSE;
	return TRUE;
}

REBOOL Gui_Widget_Get_Border(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return FALSE;
	if (wid->kind == W_GUI_WIDGET_FIELD)
		return gtk_entry_get_has_frame(GTK_ENTRY(wid->handle)) ? TRUE : FALSE;
	if (GTK_IS_SCROLLED_WINDOW(wid->handle))
		return gtk_scrolled_window_get_shadow_type(GTK_SCROLLED_WINDOW(wid->handle))
			!= GTK_SHADOW_NONE ? TRUE : FALSE;
	return FALSE;
}

void Gui_Widget_Set_Border(GUIWIDGET *wid, REBOOL on)
{
	if (!wid || !wid->handle) return;
	if (wid->kind == W_GUI_WIDGET_FIELD)
		gtk_entry_set_has_frame(GTK_ENTRY(wid->handle), on ? TRUE : FALSE);
	else if (GTK_IS_SCROLLED_WINDOW(wid->handle))
		gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(wid->handle),
		                                    on ? GTK_SHADOW_IN : GTK_SHADOW_NONE);
}


//== scrolling ================================================================

/***********************************************************************
**  A window which has not been shown has not been laid out: GTK sizes
**  a toplevel's contents only once it is on screen. On the other
**  platforms a control has its size, its lines and its rows from the
**  moment it exists, so a script building a window hidden can already
**  scroll a list to its end and read where it is. For that, a hidden
**  window is laid out here, at the size it will have, and the idle work
**  which measures lines and rows is let run - twice, since measuring
**  changes what there is to lay out.
***********************************************************************/
static void Layout_Window(GUIWIN *win, REBOOL measure)
{
	GTKWIN *gw;
	GtkRequisition req;
	GtkAllocation a;
	int round, guard;

	if (!win || !win->handle) return;
	gw = GW(win);
	if (gtk_widget_get_mapped(gw->window)) {
		// On screen: whatever is pending is done now rather than at the
		// next frame.
		if (!measure) gtk_container_check_resize(GTK_CONTAINER(gw->window));
		return;
	}

	for (round = 0; round < (measure ? 2 : 1); round++) {
		gint bar = 0;
		if (gw->menubar && gtk_widget_get_visible(gw->menubar)) {
			gint min = 0;
			gtk_widget_get_preferred_height(gw->menubar, &min, &bar);
		}
		a.x = 0; a.y = 0;
		a.width  = gw->want_w > 0 ? gw->want_w : 1;
		a.height = (gw->want_h > 0 ? gw->want_h : 1) + bar;
		gtk_widget_get_preferred_size(gw->vbox, &req, NULL);
		gtk_widget_size_allocate(gw->vbox, &a);
		if (!measure) break;
		guard = 0;
		while (g_main_context_iteration(NULL, FALSE) && ++guard < 200) {}
	}
}

#define Layout_Hidden(win) Layout_Window((win), TRUE)

static GtkAdjustment *Scroll_Adjustment(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;
	if (wid->kind != W_GUI_WIDGET_AREA && !IS_TABLE(wid)) return NULL;
	Layout_Hidden(wid->owner);
	// Laying out ran the main loop; asked again, in case that closed it.
	if (!wid->handle) return NULL;
	return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(wid->handle));
}

REBDEC Gui_Widget_Get_Scroll(GUIWIDGET *wid)
{
	GtkAdjustment *adj = Scroll_Adjustment(wid);
	gdouble span, at;
	if (!adj) return -1.0;
	span = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)
	     - gtk_adjustment_get_lower(adj);
	if (span <= 0) return 0.0;   // it all fits
	at = gtk_adjustment_get_value(adj) - gtk_adjustment_get_lower(adj);
	// Scrolled to a line or a row, the end can stop short of the range by
	// a fraction of a pixel; that is the end all the same.
	if (at >= span - 1.0) return 1.0;
	return (REBDEC)(at / span);
}

REBOOL Gui_Widget_Set_Scroll(GUIWIDGET *wid, REBDEC where)
{
	GtkAdjustment *adj = Scroll_Adjustment(wid);
	gdouble span;

	if (!adj) return FALSE;

	// The end goes through the content, not the adjustment: after the text
	// has just been replaced its height may not be measured yet, and the
	// text view scrolls to a mark once it is.
	if (where >= 1.0 && wid->kind == W_GUI_WIDGET_AREA) {
		GtkTextView *tv = GTK_TEXT_VIEW(Inner_Of(wid));
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(tv);
		GtkTextMark *mark = gtk_text_buffer_get_mark(buffer, "rebol-gui-end");
		GtkTextIter end;
		gtk_text_buffer_get_end_iter(buffer, &end);
		if (!mark) mark = gtk_text_buffer_create_mark(buffer, "rebol-gui-end", &end, FALSE);
		else gtk_text_buffer_move_mark(buffer, mark, &end);
		gtk_text_view_scroll_to_mark(tv, mark, 0.0, TRUE, 0.0, 1.0);
		Layout_Hidden(wid->owner);
		return wid->handle ? TRUE : FALSE;
	}
	if (where >= 1.0 && IS_TABLE(wid)) {
		gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj)
		                            - gtk_adjustment_get_page_size(adj));
		return TRUE;
	}
	if (where < 0.0) where = 0.0;
	span = gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj)
	     - gtk_adjustment_get_lower(adj);
	if (span <= 0) return TRUE;   // nothing to scroll
	gtk_adjustment_set_value(adj, gtk_adjustment_get_lower(adj) + span * where);
	return TRUE;
}

/***********************************************************************
**  Brings row `n` into view, scrolling as little as it takes: not at
**  all when it is in view, and otherwise so it sits at the nearer edge.
**
**  Worked out from the row's own rectangle rather than left to
**  gtk_tree_view_scroll_to_cell(), which only records the wish until
**  the next frame - so where the list is could not be read back at once.
***********************************************************************/
static void Show_Row(GUIWIDGET *wid, REBINT n)
{
	GtkTreeView *tree = GTK_TREE_VIEW(Inner_Of(wid));
	GtkAdjustment *adj = Scroll_Adjustment(wid);
	GtkTreePath *path;
	GdkRectangle r, first;
	gdouble top, page, y;

	if (!adj || !wid->handle) return;
	path = gtk_tree_path_new_from_indices(n, -1);
	gtk_tree_view_get_background_area(tree, path, NULL, &r);
	gtk_tree_path_free(path);
	path = gtk_tree_path_new_from_indices(0, -1);
	gtk_tree_view_get_background_area(tree, path, NULL, &first);
	gtk_tree_path_free(path);
	if (r.height <= 0) return;   // not measured

	// Measured from the first row: the rectangles are in the visible
	// area's coordinates, which a list not yet on screen has not moved.
	top  = gtk_adjustment_get_value(adj);
	page = gtk_adjustment_get_page_size(adj);
	y    = r.y - first.y;
	if (y < top) gtk_adjustment_set_value(adj, y);
	else if (y + r.height > top + page) gtk_adjustment_set_value(adj, y + r.height - page);
}

void Gui_Widget_Scroll_To_Item(GUIWIDGET *wid, REBINT n)
{
	if (!wid || !IS_TABLE(wid) || !Inner_Of(wid) || n < 0) return;
	Show_Row(wid, n);
}


//== focus ====================================================================

static GtkWidget *Focus_Target(GUIWIDGET *wid)
{
	if (!wid || !wid->handle) return NULL;
	switch (wid->kind) {
	case W_GUI_WIDGET_AREA:
	case W_GUI_WIDGET_TEXT_LIST:
	case W_GUI_WIDGET_LIST_VIEW:
	case W_GUI_WIDGET_TREE_VIEW:
	case W_GUI_WIDGET_DROP_LIST:
		return Inner_Of(wid);
	case W_GUI_WIDGET_DROP_DOWN:
		return Combo_Entry(wid);
	case W_GUI_WIDGET_TEXT:
	case W_GUI_WIDGET_IMAGE:
	case W_GUI_WIDGET_PANEL:
	case W_GUI_WIDGET_PROGRESS:
	case W_GUI_WIDGET_LINE:
		return NULL;
	}
	return GTKW(wid);
}

REBOOL Gui_Window_Set_Focus(GUIWIN *win)
{
	if (!win || !win->handle) return FALSE;
	gtk_window_present(GTKWINDOW(win));
	return TRUE;
}

REBOOL Gui_Widget_Set_Focus(GUIWIDGET *wid)
{
	GtkWidget *target = Focus_Target(wid);
	if (!target || !wid->owner || !wid->owner->handle) return FALSE;
	if (!gtk_widget_get_can_focus(target) || !gtk_widget_is_sensitive(target)) return FALSE;
	// Both platforms bring the window forward; so does this one.
	gtk_window_present(GTKWINDOW(wid->owner));
	gtk_widget_grab_focus(target);
	return gtk_widget_is_focus(target) ? TRUE : FALSE;
}

// The window's focus widget, whether or not the window is active - which
// is what the other platforms answer.
REBOOL Gui_Widget_Has_Focus(GUIWIDGET *wid)
{
	GtkWidget *target = Focus_Target(wid);
	if (!target) return FALSE;
	return gtk_widget_is_focus(target) ? TRUE : FALSE;
}


//== tooltips =================================================================

REBOOL Gui_Widget_Set_Tip(GUIWIDGET *wid, const REBYTE *utf8, REBCNT len)
{
	gchar *tip;
	if (!wid || !wid->handle) return FALSE;
	if (!utf8 || len == 0) {
		gtk_widget_set_tooltip_text(GTKW(wid), NULL);
		return TRUE;
	}
	tip = Dup_Text(utf8, len);
	gtk_widget_set_tooltip_text(GTKW(wid), tip);
	g_free(tip);
	return TRUE;
}

REBSER* Gui_Widget_Get_Tip(GUIWIDGET *wid)
{
	gchar *tip;
	REBSER *out;
	if (!wid || !wid->handle) return NULL;
	tip = gtk_widget_get_tooltip_text(GTKW(wid));
	if (!tip || !*tip) { g_free(tip); return NULL; }
	out = To_Rebol(tip);
	g_free(tip);
	return out;
}


//== natural size =============================================================

// One line of the widget's own font, in pixels.
static gint Line_Height(GtkWidget *w)
{
	PangoLayout *layout = gtk_widget_create_pango_layout(w, "Xg");
	gint lw = 0, lh = 0;
	pango_layout_get_pixel_size(layout, &lw, &lh);
	g_object_unref(layout);
	return lh;
}

static gint Char_Width(GtkWidget *w)
{
	PangoLayout *layout = gtk_widget_create_pango_layout(w, "0");
	gint lw = 0, lh = 0;
	pango_layout_get_pixel_size(layout, &lw, &lh);
	g_object_unref(layout);
	return lw;
}

REBOOL Gui_Widget_Natural_Size(GUIWIDGET *wid, REBINT *w, REBINT *h)
{
	GtkRequisition nat;
	gint nw, nh;

	if (!wid || !wid->handle) return FALSE;

	switch (wid->kind) {
	case W_GUI_WIDGET_BUTTON:
	case W_GUI_WIDGET_TOGGLE:
	case W_GUI_WIDGET_CHECK:
	case W_GUI_WIDGET_RADIO:
	case W_GUI_WIDGET_TEXT:
	case W_GUI_WIDGET_FIELD:
	case W_GUI_WIDGET_DROP_LIST:
	case W_GUI_WIDGET_DROP_DOWN:
	case W_GUI_WIDGET_DATE_FIELD:
		// GTK measures its own controls, frame and all.
		gtk_widget_get_preferred_size(GTKW(wid), NULL, &nat);
		nw = nat.width;
		nh = nat.height;
		break;

	case W_GUI_WIDGET_AREA: {
		// Four lines is the smallest thing that reads as multi-line.
		GtkWidget *tv = Inner_Of(wid);
		nw = 0;
		nh = Line_Height(tv) * 4 + 8;
		break; }

	case W_GUI_WIDGET_TEXT_LIST: {
		// Six rows, and the frame round them.
		GtkWidget *tree = Inner_Of(wid);
		nw = 0;
		nh = (Line_Height(tree) + 4) * 6 + 4;
		break; }

	case W_GUI_WIDGET_TREE_VIEW: {
		// Eight rows: a tree is usually browsed rather than glanced at.
		GtkWidget *tree = Inner_Of(wid);
		nw = 0;
		nh = (Line_Height(tree) + 4) * 8 + 4;
		break; }

	case W_GUI_WIDGET_LIST_VIEW: {
		// Six rows under the header, and as wide as the columns.
		GtkWidget *tree = Inner_Of(wid);
		gint n, count = (gint)gtk_tree_view_get_n_columns(GTK_TREE_VIEW(tree));
		gint row = Line_Height(tree) + 4;
		nw = 4 + 16;
		for (n = 0; n < count; n++)
			nw += Gui_List_Column_Width(wid, (REBCNT)GPOINTER_TO_UINT(
				g_object_get_data(G_OBJECT(gtk_tree_view_get_column(GTK_TREE_VIEW(tree), n)),
				                  KEY_FIELD)));
		nh = row * 6 + 4 + (gtk_tree_view_get_headers_visible(GTK_TREE_VIEW(tree)) ? row + 6 : 0);
		break; }

	default:
		return FALSE;   // no text, so no natural size to give
	}

	// An entry the user is meant to type into wants room whatever is in it
	// now - twenty characters or so, as on the other platforms.
	if (wid->kind == W_GUI_WIDGET_FIELD || wid->kind == W_GUI_WIDGET_AREA
	 || IS_TABLE(wid) || wid->kind == W_GUI_WIDGET_DROP_LIST
	 || wid->kind == W_GUI_WIDGET_DROP_DOWN) {
		gint least = Char_Width(Text_Target(wid)) * 20;
		if (nw < least) nw = least;
	}

	if (w) *w = nw;
	if (h) *h = nh;
	return TRUE;
}


//== drops ====================================================================
//
// The client area is the drop target, for the whole window; a drop lands
// on the widget under the pointer - a direct child of the window, as on
// the other platforms. Files arrive as a list of URIs, text as text: GTK
// hands over both for the same work, as AppKit does.

enum { DROP_URIS = 1, DROP_TEXT };

static void On_Drop_Data(GtkWidget *content, GdkDragContext *drag, gint x, gint y,
                         GtkSelectionData *sel, guint info, guint time, gpointer data)
{
	GUIWIN *win = (GUIWIN*)data;
	GUIDROPDATA *payload = NULL;
	GUIWIDGET *wid;
	REBHOB *target;
	gboolean ok = FALSE;

	if (!win || !win->hob || !win->handle || Gui_Window_Blocked(win)) {
		gtk_drag_finish(drag, FALSE, FALSE, time);
		return;
	}

	if (info == DROP_URIS) {
		gchar **uris = gtk_selection_data_get_uris(sel);
		gint n;
		if (uris && uris[0]) {
			payload = Gui_Drop_Payload(GUI_DROP_FILES, 256);
			for (n = 0; payload && uris[n]; n++) {
				gchar *path = g_filename_from_uri(uris[n], NULL, NULL);
				if (path) {
					gchar *utf8 = g_filename_to_utf8(path, -1, NULL, NULL, NULL);
					if (utf8) Gui_Drop_Append(payload, (const REBYTE*)utf8, (REBCNT)strlen(utf8));
					g_free(utf8);
					g_free(path);
				}
			}
			if (payload && payload->count == 0) { Gui_Drop_Free(payload); payload = NULL; }
		}
		g_strfreev(uris);
	}
	if (!payload) {
		guchar *text = gtk_selection_data_get_text(sel);
		if (text) {
			size_t len = strlen((const char*)text);
			payload = Gui_Drop_Payload(GUI_DROP_TEXT, (REBCNT)len + 1);
			if (payload) Gui_Drop_Append(payload, text, (REBCNT)len);
			g_free(text);
		}
	}

	if (payload) {
		wid = Widget_Under_Point(win, x, y, TRUE);
		target = (wid && wid->hob) ? wid->hob : win->hob;
		Gui_Queue_Drop(target, payload, x, y);
		ok = TRUE;
	}
	gtk_drag_finish(drag, ok, FALSE, time);
}

void Gui_Window_Set_Drop(GUIWIN *win, REBOOL accept)
{
	GtkWidget *content;
	if (!win) return;
	if (win->handle) {
		content = GW(win)->content;
		if (accept && !(win->flags & GUIW_ACCEPTS_DROP)) {
			GtkTargetEntry targets[] = {
				{ (gchar*)"text/uri-list", 0, DROP_URIS },
				{ (gchar*)"UTF8_STRING",   0, DROP_TEXT },
				{ (gchar*)"text/plain;charset=utf-8", 0, DROP_TEXT },
				{ (gchar*)"text/plain",    0, DROP_TEXT },
			};
			gtk_drag_dest_set(content, GTK_DEST_DEFAULT_ALL, targets,
			                  G_N_ELEMENTS(targets), GDK_ACTION_COPY);
			g_signal_connect(content, "drag-data-received", G_CALLBACK(On_Drop_Data), win);
		}
		else if (!accept && (win->flags & GUIW_ACCEPTS_DROP)) {
			gtk_drag_dest_unset(content);
			g_signal_handlers_disconnect_by_func(content, G_CALLBACK(On_Drop_Data), win);
		}
	}
	if (accept) win->flags |=  GUIW_ACCEPTS_DROP;
	else        win->flags &= ~GUIW_ACCEPTS_DROP;
}
