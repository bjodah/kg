/* dap_init(): the three debugger keymaps, created before the init file is
 * read and deactivated at once.
 *
 * Split out of src/dap_core.c because of what it links, not what it does:
 * creating a map reaches src/keymap.c, which resolves command names through
 * the command table, so every test binary that linked this object would
 * have to link the whole editor with it.  src/lsp_req.c and src/lsp_edit.c
 * are outside LSP_OBJS for exactly that reason.
 *
 * Both halves are compiled in every configuration.  The WITH_DAP=0 half
 * creates nothing: a map is a claim on key sequences and on the map table's
 * capacity, and a build whose `kg -V` says `-dap` has no business making
 * either.  An init.el that binds into a `dap` map in such a build gets
 * whatever `define-key` creates for an unknown name -- a major map nothing
 * activates, which is harmless.
 */

#include "dap.h"

#ifdef KG_USE_DAP

#include "keymap.h"

/* Two minor maps, not one, because the two halves of the debugger become
 * available at different times (doc/plans/2026-08-11-dap.md): breakpoint
 * keys work in an ordinary file buffer with no session running, while the
 * execution keys exist only while one does -- so a single map would either
 * shadow the user's keys all session long or leave F9 unbound before the
 * first launch.  `dap-info` is the major map of the debugger's own special
 * buffers.
 *
 * Every one is created inactive.  keymap_create() makes a map active from
 * the start, and an active map here would shadow global keys from startup
 * onwards, before DAP owns any state at all; the layers that turn them on
 * are subplan 02's, one per pane and per session phase.  The pointers are
 * deliberately not cached: nothing in this stage needs to reach a map
 * again, and a static pointer would outlive the keymap_reset() the tests
 * use (src/keybind.c's cached map is the cautionary case).  Later stages
 * ask keymap_find() by name.
 *
 * A full map table answers nullptr, which keymap_set_active() takes
 * without complaint; the resulting editor is one whose debugger keys do
 * not work, which is what exhausting any keymap bound looks like
 * everywhere else in kg.  test/test_keymap.c is what keeps the capacity
 * honest.
 */
static const struct {
	const char *name;
	enum keymap_layer layer;
} dap_maps[] = {
	{ "dap-breakpoint", KEYMAP_LAYER_MINOR },
	{ "dap", KEYMAP_LAYER_MINOR },
	{ "dap-info", KEYMAP_LAYER_MAJOR },
};

void dap_init(void)
{
	size_t i;

	for (i = 0; i < sizeof(dap_maps) / sizeof(*dap_maps); i++) {
		struct keymap *map
		    = keymap_create(dap_maps[i].name, dap_maps[i].layer);

		keymap_set_active(map, 0);
	}
}

#else /* !KG_USE_DAP */

void dap_init(void) { }

#endif /* KG_USE_DAP */
