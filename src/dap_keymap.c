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

#include "dap_breakpoint.h"
#include "dap_commands.h"
#include "dap_session.h"
#include "dap_ui.h"
#include "def.h"
#include "keymap.h"

#include <stddef.h>

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

/* The default table, gud-style (doc/plans/2026-08-11-dap.md, decision 1) --
 * the layout the user's own Emacs has, so that the muscle memory of
 * F5-step-in and F8-continue keeps working.
 *
 * WHICH MAP a key goes in is the whole reason there are two: F9 has to work
 * BEFORE `dap-debug`, in an ordinary file buffer with no adapter anywhere,
 * while F4-F8 and F10-F12 must not shadow a user's own F-keys outside a
 * session.  Nothing here is bound in the global map: these are minor maps,
 * they are activated per keystroke by the predicates below, and both are
 * fully rebindable from init.el -- `(define-key 'dap-mode-map "<f5>"
 * 'dap-step-in)` reaches this very map, because keymap_find()'s `-mode-map`
 * aliasing resolves the name and the map already exists.
 *
 * PageUp/PageDown join M-up/M-down on the stack walk: a user whose hands
 * are on the arrow cluster should not have to reach for Meta, and outside a
 * session the keys are the editor's own again because the map is not
 * live. */
static const struct {
	int map;
	const char *sequence;
	const char *command;
} dap_keys[] = {
	{ 0, "<f9>", "dap-breakpoint-toggle" },
	{ 0, "C-<f9>", "dap-breakpoint-temporary" },
	{ 1, "<f4>", "dap-evaluate" },
	{ 1, "<f5>", "dap-step-in" },
	{ 1, "C-<f5>", "dap-step-instruction" },
	{ 1, "<f6>", "dap-next" },
	{ 1, "<f7>", "dap-step-out" },
	{ 1, "<f8>", "dap-continue" },
	{ 1, "<f10>", "dap-until" },
	{ 1, "M-<f10>", "dap-goto" },
	{ 1, "<f11>", "dap-restart" },
	{ 1, "M-<f11>", "dap-terminate" },
	{ 1, "<f12>", "dap-many-windows" },
	{ 1, "M-<up>", "dap-frame-up" },
	{ 1, "M-<down>", "dap-frame-down" },
	{ 1, "<prior>", "dap-frame-up" },
	{ 1, "<next>", "dap-frame-down" },
	{ 2, "RET", "dap-info-select" },
	{ 2, "d", "dap-info-delete-breakpoint" },
	{ 2, "D", "dap-info-toggle-breakpoint" },
	{ 2, "t", "dap-info-toggle-breakpoints-threads" },
	/* `q` is the editor's own bury, and is bound here as well as in the
	 * special-buffer map on purpose: both maps are live in a pane, both
	 * are in the major layer, and two maps naming ONE command is a
	 * question recency answers with the same behaviour either way. */
	{ 2, "q", "quit-window" },
};

/* Whether the current buffer visits a file on disk, which is what the
 * breakpoint keys need and all they need: a breakpoint is set on a source
 * line, and neither a scratch buffer nor a debugger pane has one.
 * buf_visits_file() is the editor's own answer to that question (def.h), so
 * the keys are live in exactly the buffers a save would write.  The
 * handlers refuse such a buffer again with a sentence -- this only decides
 * whether the KEY is live. */
static bool buffer_visits_a_file(void) { return buf_visits_file(bcur()) != 0; }

/* Asked once per keystroke from kbd.c's name-keyed rule, and the ONE place
 * the debugger's maps are made live.  A bare keymap_set_active() at pane
 * creation would leave the info map live in whatever buffer the user
 * switched to next, which is the bug this shape cannot have.
 *
 * The answer is also the buffer-list map's cue to stand down: a pane is
 * read-only, that map is live in any read-only buffer, and both bind RET in
 * the major layer -- the predicate leak the LSP plan already fixed once. */
bool dap_update_mode_maps(void)
{
	bool pane = dap_ui_current_buffer_is_pane();

	keymap_set_active(
	    keymap_find("dap-breakpoint"), buffer_visits_a_file());
	keymap_set_active(keymap_find("dap"), dap_session_current() != NULL);
	keymap_set_active(keymap_find("dap-info"), pane);
	return pane;
}

void dap_init(void)
{
	struct keymap *maps[3] = { 0 };
	size_t i;

	/* Before the init file for the maps' sake, and before any command
	 * for the table's: the breakpoint table subscribes to the buffer
	 * lifecycle here, so a file opened at startup already re-anchors
	 * whatever a later session will be told about. */
	dap_breakpoint_init();
	/* And the command layer's own wiring, which subscribes the
	 * decoration projection to the same lifecycle and gives the session
	 * and the stop model somewhere to report to.  Here rather than in
	 * src/dap_core.c for this file's own reason: it reaches buffers,
	 * windows and the command table. */
	dap_commands_init();
	for (i = 0; i < sizeof(dap_maps) / sizeof(*dap_maps); i++) {
		maps[i] = keymap_create(dap_maps[i].name, dap_maps[i].layer);
		keymap_set_active(maps[i], 0);
	}
	for (i = 0; i < sizeof(dap_keys) / sizeof(*dap_keys); i++) {
		/* A map the table was full for is a nullptr keymap_bind()
		 * declines, which is what exhausting any keymap bound looks
		 * like everywhere else in kg: the keys do not work and
		 * nothing else is damaged. */
		(void)keymap_bind(maps[dap_keys[i].map], dap_keys[i].sequence,
		    dap_keys[i].command);
	}
}

#else /* !KG_USE_DAP */

void dap_init(void) { }

/* No maps were created, so there is nothing to make live -- and no pane, so
 * no other map has to stand down for one. */
bool dap_update_mode_maps(void) { return false; }

#endif /* KG_USE_DAP */
