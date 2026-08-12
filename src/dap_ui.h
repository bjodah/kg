#ifndef KG_DAP_UI_H
#define KG_DAP_UI_H

#include <stdbool.h>
#include <stddef.h>

/* The debugger's six panes, the window layout they live in, and the row
 * metadata that lets one key do six different things.
 *
 * EVERYTHING HERE IS PROJECTION.  The tables are elsewhere -- the stop model
 * (src/dap_exec.h) and the breakpoint table (src/dap_breakpoint.h) -- and
 * this module owns no state a user could lose except the two transcripts,
 * which are the only thing nobody else keeps a copy of.  So the panes are
 * rebuilt from the tables rather than edited, and every write goes through
 * buf_prepare_special_text()/buf_append_special_text(), which is what keeps
 * this file out of the mutation-gateway census by construction
 * (.ci/mutation-gateway.json).
 *
 * TWO CONTENT DISCIPLINES, NOT ONE.  `*dap-output*` and `*dap-repl*` are
 * APPEND-ONLY BOUNDED TRANSCRIPTS: the debuggee's bytes and the REPL's
 * exchanges arrive once and are never re-derived, so a repaint may not clear
 * them, the cap is charged when a byte is accepted, and past the cap kg says
 * so once and keeps draining -- an adapter must never block on its pipe.
 * `*dap-stack*`, `*dap-locals*`, `*dap-threads*` and `*dap-breakpoints*` are
 * MODEL-DERIVED REPLACEMENTS: their whole text is rebuilt from the tables on
 * every repaint, because a projection that tried to be incremental would be
 * a second copy of the table.
 *
 * ONE NOTIFICATION POINT.  Everything that can change what a pane says calls
 * dap_ui_changed(), which marks the panes dirty and nothing else; the
 * repaint happens on the next dap_ui_poll() past a ~100 ms debounce
 * (dape's `dape-ui-debounce-time`), and only for panes that are on screen.
 * dap-mode's nine hooks are the cautionary tale: granularity is added when a
 * pane measurably needs it, not before.
 *
 * ROW METADATA, BECAUSE KG HAS NO TEXT PROPERTIES.  "RET dispatches on what
 * the line is" is free in Emacs and is a mechanism here: each pane keeps a
 * bounded vector of struct dap_ui_row beside its text, plus its buffer
 * handle and a content generation.  Text and metadata are replaced as ONE
 * transaction -- built first, swapped on success, and on failure the
 * previous pane and its metadata stay exactly as they were -- and no row
 * ever stores a pointer into a JSON document, an `erow`, a variable vector
 * or a session: all four may move before the next keystroke.
 */

/* The pane names.  Spelled here rather than again in kbd.c or the tests,
 * for LSP_DIAG_BUFFER_NAME's reason: the map that gives these buffers RET
 * is keyed on the name, and two spellings are two chances to be wrong. */
#define DAP_UI_REPL_BUFFER_NAME "*dap-repl*"
#define DAP_UI_LOCALS_BUFFER_NAME "*dap-locals*"
#define DAP_UI_OUTPUT_BUFFER_NAME "*dap-output*"
#define DAP_UI_STACK_BUFFER_NAME "*dap-stack*"
#define DAP_UI_BREAKPOINTS_BUFFER_NAME "*dap-breakpoints*"
#define DAP_UI_THREADS_BUFFER_NAME "*dap-threads*"

enum dap_ui_pane {
	DAP_UI_PANE_REPL,
	DAP_UI_PANE_LOCALS,
	DAP_UI_PANE_OUTPUT,
	DAP_UI_PANE_STACK,
	DAP_UI_PANE_BREAKPOINTS,
	DAP_UI_PANE_THREADS,
	DAP_UI_PANE_COUNT,
};

/* What a line of a pane IS.  RET, `d` and `D` all dispatch on this rather
 * than on which buffer they were pressed in, which is what lets one map
 * serve all six panes. */
enum dap_ui_row_kind {
	DAP_UI_ROW_NONE = 0, /* a heading, a notice, a truncation marker */
	DAP_UI_ROW_FRAME,
	DAP_UI_ROW_THREAD,
	DAP_UI_ROW_BREAKPOINT,
	DAP_UI_ROW_SCOPE,
	DAP_UI_ROW_VARIABLE,
};

/* One line's metadata.  Everything here is a number or a flag: an index, an
 * adapter id, a line -- never an address, because the vector, the JSON and
 * the session it was derived from may all be gone by the time RET runs.
 *
 * `stable_id` is the identity RET acts on and it means what the kind says:
 * an adapter frame id, an adapter thread id, a breakpoint's LINE (its
 * source is `parent`), a scope's `variablesReference`, and -- for a
 * variable -- its index in the stop model's vector, which is exact within
 * one suspension and meaningless outside it.  `suspension_epoch` is what
 * says which suspension: a row from an older one is refused rather than
 * acted on, because every adapter measured answers a stale handle with
 * success and silently wrong data [M-4]. */
struct dap_ui_row {
	enum dap_ui_row_kind kind;
	unsigned suspension_epoch;
	int stable_id;
	int parent;
	int depth;
	bool navigable;
};

/* Bounds.  Every one of these covers an adapter-controlled collection, and
 * where one of them cuts, the pane says so on a visible "N omitted" line --
 * a truncation nobody can see is a bug report nobody can write. */
#define DAP_UI_ROWS_MAX 256
#define DAP_UI_NAME_COLS 48
#define DAP_UI_VALUE_COLS 96
#define DAP_UI_TYPE_COLS 24
/* Each transcript's cap.  Bytes are charged when they are ACCEPTED, so the
 * marker and kg's own notices do not eat the debuggee's budget. */
#define DAP_UI_TRANSCRIPT_MAX (4u * 1024u * 1024u)
/* How long a burst of changes is coalesced into one repaint. */
#define DAP_UI_DEBOUNCE_MS 100u
/* How many variable nodes may be expanded at once, and how long the
 * name-path that remembers one is. */
#define DAP_UI_EXPANDED_MAX 64
#define DAP_UI_KEY_MAX 160

/* Lifecycle.  dap_ui_init() is called from dap_commands_init(), before the
 * init file is read; dap_ui_reset() drops every pane handle, transcript
 * budget and expansion, and is what a test calls between cases. */
void dap_ui_init(void);
void dap_ui_reset(void);

/* How dap_ui_poll() is reached from dap_poll().  A function pointer rather
 * than a call, because of what the two objects LINK: src/dap_core.c is in
 * every test binary and this file is not (it reaches buffers, windows and
 * the command table), so a direct call there would drag the editor into
 * every suite.  Defined in src/dap_core.c in both build configurations and
 * installed by dap_commands_init(); NULL is a perfectly good answer. */
void dap_set_ui_poll(int (*fn)(void));

/* THE ONE NOTIFICATION POINT.  Cheap, and safe to call from inside a
 * response callback: it sets a flag and a deadline and touches no buffer. */
void dap_ui_changed(void);

/* The debounced repaint, called from dap_poll() through the hook below.
 * Nonzero when something on screen changed, dap_poll()'s convention.
 * Repaints nothing while a minibuffer prompt is open: a pane that created a
 * buffer under a prompt would rearrange the world the prompt is standing
 * in. */
int dap_ui_poll(void);

/* Repaint now, past the debounce, for a command whose whole point is that
 * the pane says something different afterwards. */
void dap_ui_refresh_now(void);

/* One `output` event's bytes, in arrival order and across categories.
 * `category` is the protocol's: `stdout` and `stderr` land in
 * `*dap-output*`, `console` in `*dap-repl*`, and `telemetry` never arrives
 * (the client drops it).  Appended AS BYTES: debugpy splits one `print`
 * across two events mid-line, so an event is not a line [M-12]. */
void dap_ui_output(const char *category, const char *text, size_t len);

/* The REPL's two halves: what the user asked, and what came back.  Both are
 * appended to `*dap-repl*`, which is read-only -- the input is the
 * minibuffer (src/dap_commands.c), because a prompt line inside the buffer
 * would need an editable-tail mode kg does not have. */
void dap_ui_repl_echo(const char *expression);
void dap_ui_repl_answer(const char *expression, bool error, const char *text);

/* Show a pane, creating it if it is not there, and put it in another
 * window.  False when the buffer table had no room. */
bool dap_ui_show(enum dap_ui_pane pane);

/* The layout.  dap_ui_layout_toggle() is `dap-many-windows`:  the first one
 * saves the user's configuration exactly once and enters the grid, the
 * second restores it and invalidates the snapshot, and a later one saves the
 * THEN-CURRENT layout rather than resurrecting the first.  A restore while a
 * prompt is open is deferred to the next poll rather than done underneath
 * it. */
void dap_ui_layout_toggle(void);
/* A session has begun: put the grid back up if that is what the user last
 * asked for.  The PREFERENCE is per-session and this is what reads it; the
 * snapshot is not the preference and dies on every restore. */
void dap_ui_layout_session_started(void);
/* The session is over: put the windows back, but only if the grid is what
 * is on screen. */
void dap_ui_layout_session_ended(void);
bool dap_ui_layout_active(void);

/* The `dap-info` map's commands, dispatched on the row metadata rather than
 * on which pane they were pressed in. */
void dap_ui_info_ret(void);
void dap_ui_info_delete(void);
void dap_ui_info_toggle_enable(void);
void dap_ui_info_toggle_breakpoints_threads(void);

/* Whether the current buffer is one of the six.  kbd.c asks this through
 * the facade (src/dap.h) to make the `dap-info` map live, and the answer is
 * also the buffer-list map's cue to stand down: both bind RET in the major
 * layer, and the read-only-buffer predicate the LSP work fixed would
 * otherwise make both live in a read-only pane. */
bool dap_ui_current_buffer_is_pane(void);

/* ------------------------------ test seams ---------------------------- */

/* What the metadata says about one line of a pane, without going through a
 * keystroke.  False when the pane has no such row. */
bool dap_ui_test_row(enum dap_ui_pane pane, size_t row, struct dap_ui_row *out);
size_t dap_ui_test_row_count(enum dap_ui_pane pane);
/* The pane's content generation.  Bumped once per completed transaction, so
 * a repaint that changed nothing still proves it ran. */
unsigned dap_ui_test_generation(enum dap_ui_pane pane);
/* How many bytes of the transcript's budget have been charged, and whether
 * the truncation marker has been printed. */
size_t dap_ui_test_transcript_bytes(enum dap_ui_pane pane);
bool dap_ui_test_transcript_truncated(enum dap_ui_pane pane);
/* Whether the variable node at `index` of the stop model is expanded. */
bool dap_ui_test_expanded(size_t index);

#endif /* KG_DAP_UI_H */
