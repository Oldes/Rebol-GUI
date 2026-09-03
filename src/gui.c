//
// Project: Rebol/GUI extension
// SPDX-License-Identifier: Apache-2.0
// ===========================================================================
// Entry points for the GUI extension.
//
//   REB_EXT defined ... standalone gui-x64.rebx
//   REB_EXT absent .... compiled into the host
//
// One-time setup lives in Gui_Init(), called from the generated `_init`
// command when the module body evaluates - not from the entry points, so
// that `Options: [delay]` can postpone it until the module is imported.
//

#include "gen-gui.h"
#include "gui.h"

#ifdef REB_EXT
// Standalone builds are their own binary and must supply the storage.
// Embedded builds use host-lib.c's definition, declared extern by reb-lib.h.
RL_LIB *RL;
#endif

static char *init_block = GUI_EXT_INIT_CODE;

// Symbols of the registered handle types. Declared in the generated header
// (from the spec's `c-header:`), defined here.
REBCNT Handle_GuiWindow = 0;
REBCNT Handle_GuiWidget = 0;


// Registers both handle types and does whatever the platform needs before
// the first window exists (DPI awareness on Windows, NSApplication on
// macOS). Runs when the module body evaluates, so it happens at the same
// point in both build modes - and only on first import under `delay`.
//
// Returns plain TRUE/FALSE, NOT an RXR_* code: RXR_FALSE is 3, which is
// truthy in C. The generated handler maps the result onto RXR_TRUE/RXR_FALSE.
int Gui_Init(void) {
	REBHSP spec;

	// Both callbacks take the HOB, not the raw data pointer - they need
	// hob->flags to drop the lock a live native object holds on its handle.
	spec.size     = sizeof(GUIWIN);
	spec.flags    = HANDLE_REQUIRES_HOB_ON_FREE;
	spec.free     = GuiWindow_free;
	spec.get_path = GuiWindow_get_path;
	spec.set_path = GuiWindow_set_path;
	spec.mold     = GuiWindow_mold;

	Handle_GuiWindow = RL_REGISTER_HANDLE_SPEC(cb_cast("GUI-WINDOW"), &spec);
	if (Handle_GuiWindow == 0) return FALSE;

	spec.size     = sizeof(GUIWIDGET);
	spec.flags    = HANDLE_REQUIRES_HOB_ON_FREE;
	spec.free     = GuiWidget_free;
	spec.get_path = GuiWidget_get_path;
	spec.set_path = GuiWidget_set_path;
	spec.mold     = GuiWidget_mold;

	Handle_GuiWidget = RL_REGISTER_HANDLE_SPEC(cb_cast("GUI-WIDGET"), &spec);
	if (Handle_GuiWidget == 0) return FALSE;

	Gui_Init_Platform();
	return TRUE;
}


#ifdef REB_EXT

/***********************************************************************
**  Standalone extension library
***********************************************************************/

RXIEXT const char *RX_Init(int opts, RL_LIB *lib) {
	REBYTE ver[8];
	RL = lib;
	RL_VERSION(ver);

	if (MIN_REBOL_VERSION > VERSION(ver[1], ver[2], ver[3])) return 0;
	if (!CHECK_STRUCT_ALIGN) {
		trace("CHECK_STRUCT_ALIGN failed!");
		return 0;
	}
	return init_block;
}

RXIEXT int RX_Quit(int opts) {
	// Windows still open at shutdown are destroyed here; their handles are
	// about to go away with the interpreter, so nothing can reach them.
	Gui_Quit_Platform();
	return 0;
}

// Reports the RL_API ABI this was built against, so `load-extension`
// can refuse an incompatible host. An absent symbol means ABI 0.
RXIEXT int RX_Abi(void) {
	return RL_ABI_VERSION;
}

// Resolved by name, so the spelling is fixed. The bounds-checked
// dispatcher is generated into gen-gui.c.
RXIEXT int RX_Call(int cmd, RXIFRM *frm, void *ctx) {
	return Gui_RX_Call(cmd, frm, ctx);
}

#endif
