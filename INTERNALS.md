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

Windows runs `PROCESS_SYSTEM_DPI_AWARE`. `SPI_GETNONCLIENTMETRICS` returns the
message font already scaled for the display, so the backend converts between
logical and device units at its boundary, events included. AppKit works in
points and converts nothing. Per-monitor awareness would need
`WM_DPICHANGED`.

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
- **Progress:** `PBM_SETPOS` animates increases, so the backend steps one past
  and back.
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

Windows positions go through `To_Logical`, so they share the window-offset
space, and `scale` is `Gui_DPI / 96` for every screen - correct only while the
process is system-DPI aware. Making it per monitor needs
`PER_MONITOR_AWARE_V2` and `WM_DPICHANGED` (step 2). The name comes from
`EnumDisplayDevices` on the monitor under the output; `QueryDisplayConfig`
would give the EDID friendly name more reliably.

macOS flips `frame` / `visibleFrame` against the menu-bar screen with
`Screen_Height()`, as window offsets are, and asks `localizedName` by selector
because the SDK floor is 10.13.