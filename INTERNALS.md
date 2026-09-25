# Rebol/GUI - implementation notes

For people working on the extension itself. Users want [README.md](README.md).

## What it needs from the interpreter

`Needs:` in `src/gui.reb` (also the source of the C side's
`MIN_REBOL_VERSION` check). The core changes this extension depends on:

- **`RL_Register_Device`, `RL_Do_Device`, `RL_Port_State`** - the event model
  below is built on them.
- **Win32 `Query_Events` dispatches the message it removed.** `OS_Wait` uses it
  as its timing method; it used to `GetMessage` a message and dispatch it only
  under `REB_VIEW` with a View window focused, dropping everything else. With
  an extension owning windows, `wait` then ate a message per call.
- **Handle comparison.** `=` compares handle type, `==` identity;
  `Cmp_Handle` orders context handles before plain ones.
- **Event `code` as a word** (`EVF_HAS_SYM`), used by `menu-select` - see
  `core-menu-word.md`.
- **`date!` with its time** (`RXIARG.datetime`, 3.22.9) - used by
  `date-field`'s `value`.
- Already present: `image!` crossing the ABI ignores the series index, and a
  released context handle still answers `/type`.

## The event model

**Who pumps the OS queue?** The host, from inside `wait`. The extension
registers a device with `RDO_AUTO_POLL`, so `OS_Wait` polls it even with
nothing pending, and that poll calls `Gui_Pump`. One sleep serves both queues.
The earlier design slept inside the extension, and nothing serviced Rebol's own
queue while a window was open.

**How does `wait` return early?** The poll pushes an event with `RL_Event`,
which wakes `WAIT`. It cannot signal through its return code: the poll always
answers `DR_DONE`. The `REBREQ` an `RDC_POLL` handler gets lives on
`OS_Poll_Devices`' C stack, and any other answer makes `OS_Do_Device` attach it
to the device's pending list, where it dangles after the frame is gone.
(`DR_PEND` also makes `OS_Wait` return -1 on every poll, so `wait` spins.)

The pushed event is delivered to the `gui` scheme's port, opened on import as
`gui/event-port`. It is a doorbell, not a channel: its `awake` returns true,
`read` answers how many events are waiting, and the GC marks it while an event
of its own is queued. It rings once per batch - on the first poll that finds the
queue non-empty, and not again until `poll-events` drains it. The test is
"anything waiting", not "anything new since the pump", because events also
arrive outside a pump (a programmatic resize, a modal OS drag loop). Ringing
from `Gui_Queue_Event` is not allowed: it can run inside such a modal loop,
where growing a Rebol series is unsafe.

Diagnostics: `gui-device` (the id the host assigned; positive means
accepted), `gui-device-polls`, `gui-device-events` (wakes pushed),
`gui-device-pumps`, `gui-device-messages`.

GUI events stay in the extension's own queue and come out of `poll-events` as
`event!` values naming their source through `EVM_HANDLE`, so the host knows
nothing about windows.

**Why not a UI thread.** `WM_PAINT` on an image widget reads pixels straight
from a Rebol series, which today only happens while the interpreter is inside
this extension; on another thread the GC could move it. Win32 window ownership
is per thread, so every command would need marshalling. AppKit requires the
main thread anyway.

## Backend contract

Everything above the OS is shared. A backend implements the `Gui_*` functions
in `gui.h`; no command, accessor or queue code is platform-aware.

A window keeps an intrusive, flat list of all its widgets at any depth. That is
how closing a window turns every widget handle into a removed one, and how
radio groups are settled.

A container's handle context keeps one GC-marked block of two: the payload
(the `image!` for an image widget, the menu block for a window, `none`
otherwise) and the children block. Only four functions in `gui-commands.c`
know this layout.

`remove-widget` on a container destroys the children first, depth-first:
Windows would take them anyway, but on macOS the extension holds a reference to
every control and would leak them.

## Painting

Creating a widget marks it dirty; it is painted by the next pump. On Windows
`redraw` repaints at once (`Repaint_Widget`). On macOS all drawing happens
in `Gui_Pump`, because the extension never runs `-[NSApplication run]`, which
is what normally displays dirty views. Forcing a display at other times is
unreliable - AppKit can clear needs-display without painting and the view stays
blank.

No macOS view fills its background when it has nothing to show. An opaque
panel or empty image widget once hid every widget created before it.

## Coordinates and DPI

Windows runs `PER_MONITOR_AWARE_V2` where available (Windows 10 1703+), set for
the process and, in case a host manifest already fixed that, for the thread
that creates the windows. `Per_Monitor` is only true when the thread really is
per-monitor aware and `GetDpiForWindow` exists; otherwise the fallback is
`PROCESS_SYSTEM_DPI_AWARE` and every DPI below is `Gui_DPI`, the system DPI.

- Every conversion takes a DPI: `Dpi_Of(hwnd)` for anything in a window
  (controls share their window's), `Dpi_Of_Monitor` for the desktop.
  `Metric()` and `Adjust_Rect()` use the `ForDpi` calls.
- Fonts are cached per DPI. The shell's message font from
  `SPI_GETNONCLIENTMETRICS` is scaled for the system DPI, so `Font_For` rescales
  it; `Default_Font_At(dpi)` replaces the single default font.
- `WM_DPICHANGED` runs `Rescale_Window`: every widget's box is scaled old->new
  in its parent, every font remade at the same point size, and the window keeps
  its logical client size at Windows' suggested position. The old DPI is kept in
  a window property (`RebolGuiDpi`), because `GetDpiForWindow` already answers
  the new one when the message arrives.
- Desktop coordinates (window offsets, screens) have no common logical unit
  across monitors of different scale. Each monitor keeps its physical top-left
  corner and is scaled from there (`Desk_To_Logical`); `Logical_To_Screen` finds
  the monitor whose logical rectangle holds the point. Rectangles can only
  shrink, so they never overlap. Qt uses the same rule.

AppKit works in points and converts nothing.

Cocoa flips offsets against the menu-bar screen; the content view answers
`isFlipped`. A slider's own coordinates are not flipped.

## Widgets

- **Fonts** are read back from the control, not stored. Windows caches one
  `HFONT` per distinct face and frees them at shutdown. macOS converts
  families through `NSFontManager` and colours buttons through an attributed
  title, rebuilt when text or font changes.
- **Text colour** is stored per widget, because Win32 asks the parent at paint
  time and keeps none on the control.
- **Read-only** is stored too: macOS merges enabled and editable, so a
  disable/enable cycle must be able to restore it.
- **Suppressed `change`**: Windows suppresses the `EN_CHANGE` that
  `SetWindowText` sends; macOS's `setStringValue:` never calls the delegate.
- **Area scroll** is a fraction because Win32 `EDIT` scrolls in lines and
  Cocoa in points. `'end` on macOS uses `scrollRangeToVisible:`, which forces
  layout after the string was just replaced.
- **Area on macOS** is an `NSScrollView` handle with the `NSTextView` inside.
- **Transparency on Win32:** over a colour, the control is handed that colour
  (themed checks and radios fade through `BufferedPaintAnimation` and would
  vanish if given nothing). Over an image widget, the control's `WM_PAINT` is
  taken over: parent via `WM_PRINTCLIENT`, then the control. Such a control does
  not animate.
- **Transparent window:** macOS uses a non-opaque `NSWindow`. Win32 uses
  `WS_EX_LAYERED` with a magenta colour key, because `UpdateLayeredWindow`
  alpha does not composite child windows.
- **Borderless:** `WS_POPUP` on Windows; `NSWindowStyleMaskBorderless` on
  macOS, which is why `resizable?` refuses there. Every macOS window subclass
  answers `YES` to `canBecomeKeyWindow` so fields in a borderless window accept
  typing.
- **Panels / group boxes** draw their own frame. `BS_GROUPBOX` is not a
  container; `NSBox` insets its content. The panel's window procedure forwards
  `WM_COMMAND`, scroll and control-colour messages to the window.
- **Radio groups** are the extension's own. Windows uses `BS_RADIOBUTTON`
  (not auto). On macOS AppKit clears siblings sharing an action, so the button's
  action is detached for each state write. Redundant `BM_SETCHECK` is skipped
  because it restarts the theme fade.
- **Progress:** `PBM_SETPOS` is sent as is, so a themed bar slides to a higher
  value (the control's own animation). Decreases jump, and `dark-controls?`
  windows paint the bar themselves at its final position, with no animation.
- **Slider:** Windows works in 1/1000 steps and flips vertical trackbars. On
  macOS `NSSliderCell`'s tracking is a modal loop that starves the pump, so
  `RebolGuiSlider` tracks the mouse itself and calls the cell's
  `startTrackingAt:` / `continueTracking:at:` / `stopTracking:at:` hooks so the
  pressed knob still renders.
- **Mouse coordinates:** every mouse event is in window client coordinates.
  Windows maps a child's `lParam` up with `MapWindowPoints` in
  `Queue_Widget_Mouse`; macOS converts into the (flipped) content view with
  `Client_Point`. `widget/at` is computed in the shared layer by adding
  `offset`s up the `parent` chain.
- **Positions at fractional scales (Windows):** every conversion rounds on its
  own, so a pointer on a control's last pixel row can come out one unit past the
  control's rounded box (and nested `at` sums add a unit per level). Mouse
  positions are clamped into the box of the window or control Windows
  delivered them to (`Clamp_To_Widget` / `Clamp_To_Window`) - except while the
  mouse is captured, when a drag may legitimately be anywhere.
- **Mouse moves:** on Windows `Nav_Proc`, which every control has, reports
  `WM_MOUSEMOVE` for all of them; labels answer `HTTRANSPARENT`, so their
  container reports instead. On macOS only the content view has a tracking
  area; its `mouseMoved:` picks the deepest widget under the point with
  `Widget_Under_Point`, skipping labels and disabled controls to match Win32.
  AppKit also sends tracking-area moves down the key window's responder chain
  (first responder: the content view), so `Move_Point` drops moves for other
  windows and a second copy of the same move.
- **Press events (`down`/`move`/`up`):** Windows reports them from
  `Nav_Proc` while the control holds the capture; `WM_CAPTURECHANGED` closes a
  press that loses it. `up` goes out before a button's `BN_CLICKED` and after a
  trackbar's own `WM_LBUTTONUP`, so it is the last event of a drag. On macOS
  `NSButton` tracks in a modal loop too, so `RebolGuiButton` tracks the press
  itself (highlight while inside, `setNextState` for a check, then `clicked:`
  directly) - otherwise `down` reaches Rebol only together with `up`.
- **Drop-down:** Windows' `COMBOBOX` height includes the list, so the backend
  adds it and reports `CB_GETITEMHEIGHT`. macOS adds `NSMenuItem`s directly
  so duplicate titles survive.
- **Text-list:** Windows uses a `LISTBOX` with `WS_EX_CLIENTEDGE`,
  `LBS_NOTIFY` and `LBS_NOINTEGRALHEIGHT`. It has `WS_VSCROLL` but not
  `LBS_DISABLENOSCROLL`, so the bar shows only while needed. The item
  functions pick `LB_*` or `CB_*` messages by kind. `LB_SETCURSEL` notifies no
  one, so `index:` raises no `change`. macOS uses a one-column, cell-based
  `NSTableView` in an `NSScrollView` with `autohidesScrollers`. The table is
  its own data source, holds the strings in an `NSMutableArray`, and
  suppresses `change` while the script sets the selection. `setFont:` is
  overridden to set the column cell's font and the row height.
- **`scrollable?` off:** the list box keeps its creation style and calls
  `SetScrollInfo` whenever its items or size change, which sets `WS_VSCROLL`
  again. So the bit stays, and `Nav_Proc` clears it only around
  `WM_NCCALCSIZE`, `WM_NCPAINT` and `WM_NCHITTEST`. The frame then has no
  scroll bar, the control is not recreated, and `LB_SETTOPINDEX` still
  scrolls. `WM_MOUSEWHEEL` goes to `DefWindowProc`, which passes it to the
  parent. `scroll` on a list works in rows (`LB_GETTOPINDEX`), not from the
  scroll bar. On macOS, `setHasVerticalScroller:` is toggled and
  `scrollWheel:` is passed to the scroll view's next responder.
- **`/secure` field:** `GUI_TEXT_SECURE` (state bit 2) is set before the
  control is created. Windows adds `ES_PASSWORD`. macOS creates a
  `RebolGuiSecureField`, a subclass of `NSSecureTextField`. Giving the plain
  field an `NSSecureTextFieldCell` does not work: the cell throws on the
  first click unless its field editor's delegate is an `NSSecureTextField`.
  Both field classes share their methods through the `TEXT_FIELD_BODY`
  macro. The secure field editor masks the text, refuses copy and cut, and
  turns on secure keyboard input. This cannot be changed after creation, so
  `secure?` is read-only.
- **`/flat` and `edge` on an entry or a list:** these read the native control
  back rather than a state bit, because `state` bit 1 already means read-only
  on a field and fixed on a list. On Windows this is `WS_EX_CLIENTEDGE`
  toggled with `SWP_FRAMECHANGED`; natural size and `Paint_Dark_Edge` both
  check the bit. On macOS a field's bezel is switched off with
  `setDrawsBackground:YES` so `background` still shows, and an area's or a
  list's scroll view uses `NSNoBorder`. `/flat` is applied before
  `Attach_Widget`, so a natural size leaves no room for a missing border.
- **Date-field:** since 3.22.9 a `date!` crosses the extension boundary as
  `RXIARG.datetime`: the 32 `REBYMD` bits in `date`, and nanoseconds in
  `time` (`NO_TIME` for none). The bits are unpacked by shifts, not through
  the bit-fields, whose declared order depends on `ENDIAN_LITTLE`.
  `Date_From_Arg` keeps the field's time of day when none is given, wraps a
  time into the day, and ignores the zone. A field without `/time` reads
  back with `NO_TIME`. Backends exchange `GUIDATE` (year, month, day,
  nanoseconds since midnight, all local). Windows: `DATETIMEPICK_CLASS`
  (`ICC_DATE_CLASSES`), `DTS_SHORTDATECENTURYFORMAT`, and for `/time` a
  `DTM_SETFORMAT` built from `LOCALE_SSHORTDATE` + `LOCALE_SSHORTTIME`.
  `DTN_DATETIMECHANGE` and `NM_SETFOCUS`/`NM_KILLFOCUS` are handled in the
  window's `WM_NOTIFY`. The sender is found by walking the window's widget
  list, since a date picker's calendar also notifies. `DTM_SETSYSTEMTIME`
  notifies too, so `Setting_Date` suppresses it. Natural size comes from
  `DTM_GETIDEALSIZE`. macOS: `NSDatePicker` in text-field-and-stepper style,
  local time zone, target/action for `change` (not sent for
  `setDateValue:`), and first-responder overrides for focus. An impossible
  date is refused on both platforms: Windows refuses it itself, and macOS
  would roll it over, so the result is checked.
- **Integer `scroll` on a text-list:** the shared layer clamps it to the
  items and calls `Gui_Widget_Scroll_To_Item` 0-based. Windows compares it
  with `LB_GETTOPINDEX` and the rows that fit, then sets the top index only
  when the item is outside the view. macOS uses `scrollRowToVisible:`, which
  already works that way.
- **Image widget:** the pixel pointer and size are read at every paint. BGRA
  is a 32-bit `BI_RGB` DIB and
  `kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little`. `RXIARG`'s image
  view overlaps `index` with the dimension bitfields, so the index must be
  ignored on both sides.

## Keyboard (Windows)

The host dispatches the OS queue itself (`Query_Events` in `dev-event.c`), so
key messages never reach `Gui_Pump`. Every control is therefore subclassed and
calls `TranslateAccelerator` and `IsDialogMessage` from inside; the pump's copy
is a fallback for programs that never `wait`.

- `IsDialogMessage` is re-entrant when the control claims the key
  (`DLGC_WANTALLKEYS` on an area): a flag makes the second visit fall through.
- Tab is handled with `GetNextDlgTabItem` directly, so Tab cannot be trapped in
  an area.
- Enter and Escape are held back: the dialog manager answers them with
  `IDOK`/`IDCANCEL` `WM_COMMAND`s, which look like menu picks.
- `WS_GROUP` is set on every control except a radio following a radio of the
  same group id, so dialog-manager groups match the extension's.
- Accelerator lookup runs only for keyboard messages; the window is found by
  class name, not `GWLP_USERDATA`.

## Menus

Windows: `SetMenu` re-splits the window, so it is grown by the measured loss
(a bar can wrap). macOS: the bar is swapped in when a window becomes key, and
an application menu is always inserted so the first user menu is not taken as
it; Quit reports `close` because terminating would end the interpreter.

## Drops

Windows registers an OLE `IDropTarget`; `WM_DROPFILES` is only a courtesy
Explorer extends, and other sources refuse the drop without a registered
target. If COM is already multi-threaded (`RPC_E_CHANGED_MODE`) the fallback is
`DragAcceptFiles`. macOS registers the content view as a dragging destination.
Content and target live in the drop handle's GC-marked slot.

## macOS class names

Objective-C classes share one process-wide namespace, so names carry
`GUI_CLASS_PREFIX` (the nest gives the standalone build `RebolGuiRebx`).
Two copies with the same prefix in one process collide.

## Screens

A screen handle stores only a key: the GDI device name on Windows
(`MONITORINFOEX.szDevice`; an `HMONITOR` can change with the display
configuration) and the `CGDirectDisplayID` on macOS (`NSScreen` objects are
replaced). Every read enumerates the displays again and matches by key, so a
handle is never stale, and reads `none` once its display is gone.

`gui-commands.c` keeps a 16-entry table of live screen handles so the same
display always comes back as the same handle and `==` works. The table holds
no reference: a collected handle's free callback removes itself.

Windows positions use the per-monitor rule above, so they share the
window-offset space, and `scale` is the monitor's own DPI. The name comes from
`EnumDisplayDevices` on the monitor under the output; `QueryDisplayConfig`
would give the EDID friendly name more reliably.

macOS flips `frame` / `visibleFrame` against the menu-bar screen with
`Screen_Height()`, as window offsets are, and asks `localizedName` by selector
because the SDK floor is 10.13.

## Tracking the mouse outside (`track-mouse`)

No OS reports pointer movement to a program that does not own the window under
it, so after every pump `Gui_Track_Pointer` asks `Gui_Pointer_At` where the
pointer is (`GetCursorPos` / `[NSEvent mouseLocation]`). It skips points over
one of our windows (the topmost window under the point: `WindowFromPoint` + root
class name / `windowNumberAtPoint:` + `windowWithWindowNumber:`) and presses in
progress (`GetCapture` / pressed buttons while active), and queues a move only
when the position or screen changed. The device poll keeps running with no
window open while tracking is on.

The pump must not allocate, so the event carries the screen's key
(`GUIEVT.screen`) with a NULL source; `poll-events` turns it into the screen
handle via `Screen_Handle`, the same one `screens` returns. Modifiers come from
`GetAsyncKeyState`, since `GetKeyState` is stale while another program has the
focus.

## `enter` and `leave`

Worked out in the shared layer (`Hover_To` in `gui-commands.c`), not asked of
either platform: every `move` already names the deepest source under the
pointer, so the hovered thing changes exactly when a move arrives from another
source. `Hover` remembers the source (a handle, or a screen key) and its last
position; `leave` for it and `enter` for the new one are queued before the move.

The backends only report the pointer leaving every window of ours:
`WM_MOUSELEAVE` (asked for with `TrackMouseEvent` on every move, in the window
procedure and `Nav_Proc`; ignored while the pointer is still over the same
top-level window, which is what a move onto a child looks like) and
`mouseExited:` on the content view's tracking area. Both skip presses in
progress. A handle going away forgets its hover (`Forget_Hover` in
`Release_Handle`); turning `track-mouse` off leaves a hovered screen.

## Tooltips

macOS: `NSView.toolTip`, on the text view as well for an area (the scroll view
is covered by it).

Windows: one `TOOLTIPS_CLASS` control per top-level window, created on the first
tip and owned by the window (kept as the `RebolGuiTip` property). Tools are
`TTF_IDISHWND` keyed by the control's HWND, fed by `TTM_RELAYEVENT` from
`Nav_Proc` and the window procedure rather than `TTF_SUBCLASS`, which would add
comctl32's own subclass on top of ours. A label never gets the mouse
(`HTTRANSPARENT`), so its tip is a rectangle tool on its container, moved with
the label (`Tip_Follow` from `Gui_Widget_Set_Box` and `Rescale_Window`). The
tool is removed before a control is destroyed so a reused HWND cannot inherit
it. `TTTOOLINFOW_V2_SIZE` keeps it working without a comctl32 v6 manifest.

## Light and dark (`theme-change`)

`Gui_Theme_Changed` in the shared layer queues `theme-change` with the word
`light`/`dark` (the `theme` word list) as a symbol code, the way `menu-select`
carries its word, and only when it differs from `GUIW_DARK` - the appearance
last reported for that window, set when it opens.

Windows: the setting is `AppsUseLightTheme` under `HKCU\...\Themes\Personalize`
(read with `RegGetValueW`, loaded late from advapi32). A switch arrives as
`WM_SETTINGCHANGE` with `"ImmersiveColorSet"`, several times over. The title bar
follows through `DwmSetWindowAttribute` (attribute 20, or 19 before 20H1).

The rest follows only for a window with `dark-controls?` (`GUIW_DARK_CONTROLS`),
because the result is incomplete. `Dark_For(win)` is `Dark_Now` (system-wide)
and the flag; it picks `Default_Window_Color`/`Default_Text_Color` and a lighter
entry fill in place of the system colours, which do not change with the
appearance. Controls get `SetWindowTheme` classes (`Theme_Control`):
`DarkMode_Explorer` for buttons, toggles, checks, radios, fields and areas,
`DarkMode_CFD` for drop-downs - undocumented, present since 1809. Applied at
creation (`Subclass_For_Nav`), when the flag changes, and on a real switch
(`Theme_Window`), before the event.

Around the styles: uxtheme's ordinal-only dark-mode calls (1809+, checked with
`RtlGetVersion`) - `SetPreferredAppMode(AllowDark)` once, `AllowDarkModeForWindow`
per window and control (the scroll bars and popup menus need it),
`FlushMenuThemes` after a change. The menu BAR never goes dark on its own, so a
dark window answers the undocumented `WM_UAHDRAWMENU`/`WM_UAHDRAWMENUITEM` and
paints over the light line under the bar after `WM_NCPAINT`/`WM_NCACTIVATE`.
A field's or an area's `WS_EX_CLIENTEDGE` is drawn in light system colours
whatever the style, so `Nav_Proc` repaints that ring dark after the base
`WM_NCPAINT` (`Paint_Dark_Edge`); `Theme_Window` forces `SWP_FRAMECHANGED` so
edges and the bar repaint on a switch.

What no style covers is drawn by hand, only for a dark window: a themed check or
radio ignores the text colour, so its `NM_CUSTOMDRAW` is answered with the
theme's own glyph (`DrawThemeBackground`, in the control's state) and the label
drawn here (`Dark_Check_Draw`); a trackbar's channel and thumb through its item
custom draw (`Dark_Slider_Draw`); and a progress bar, which has no custom draw,
painted whole in `Nav_Proc` (`Dark_Progress_Paint`). Panels and image widgets
forward `WM_NOTIFY` so custom draw reaches the window. A themed check/radio draws its label in the
theme's colour, ignoring `WM_CTLCOLORSTATIC`; an always-on version that removed
their theme instead (for readable text) looked worse and was dropped.

macOS: `viewDidChangeEffectiveAppearance` on the content view, and the answer
from `bestMatchFromAppearancesWithNames:` - both by selector and by appearance
name, since the SDK floor (10.13) predates them.