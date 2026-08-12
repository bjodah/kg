#ifndef KG_DAP_EXEC_H
#define KG_DAP_EXEC_H

#include <stddef.h>

/* Running, stopping, and what kg believes while a program is suspended.
 *
 * The session below (src/dap_session.h) owns the handshake and the teardown
 * lattice; this owns the other half of a debugger -- the waterfall that
 * turns one `stopped` event into a thread list, a frame, a source line and
 * a set of variables, and the requests that let the program go again.  It
 * holds no editor state: a file is visited, a message is shown and a
 * relaunch is started through struct dap_exec_hooks, so a unit test drives
 * the whole model against a real adapter with no editor linked.
 *
 * TWO COUNTERS, AND THEY GUARD DIFFERENT THINGS.
 *
 * The SUSPENSION EPOCH is [M-4], and it is mandatory rather than defensive:
 * after a resume and a re-stop both measured adapters answer a stale
 * `variablesReference` or `frameId` with `success:true` and silently wrong
 * data.  No error ever says "stale".  So every continuation in the
 * waterfall -- and `evaluate` with it -- carries the epoch it was sent
 * under and a reply whose epoch has moved on is dropped.  It bumps BEFORE
 * any resume-implying send, on an accepted `stopped`, on an unsolicited
 * `continued` and on session end.  A bump only-on-stop would leave the
 * window between a continue and the next stop unguarded, which is exactly
 * where a late reply lands.
 *
 * The SELECTION GENERATION guards what the epoch cannot see: two frames of
 * ONE stop.  A user who selects frame B while frame A's `scopes` is in
 * flight has not resumed anything, so the epoch still agrees -- and frame
 * A's locals must still not paint.  Every selection change bumps it.
 *
 * The THREADS REFRESH SERIAL is the third of the same family and the
 * narrowest: a `threads` reply is accepted only when it answers the NEWEST
 * request, so a second stop mid-flight cannot be overwritten by the first
 * stop's late answer (dape's threads-update-handle, upgraded from a bool).
 *
 * THE ORDER IS DAPE'S, AND EVERY STEP OF IT WAS MEASURED [M-10, M-11]:
 * record the reason as an OPEN STRING (lldb-dap's stopOnEntry says
 * "exception" and "signal SIGSTOP"; nbcode's step says "pause" -- nothing
 * may switch on it); bump the epoch; reset the frame selection; mark
 * threads with the SPEC defaults, where an absent or false
 * `allThreadsStopped` marks only the named thread and `threadId` itself may
 * be absent -- with none, wait for `threads` and pick, never `stackTrace`
 * with a guess; honour `preserveFocusHint`; then `threads`, then
 * `stackTrace` for ONE frame, then a bounded page only if that frame cannot
 * be navigated to, then `scopes`, then `variables`.
 *
 * The deliberate asymmetry between the two all-threads flags is real and is
 * implemented here: absent `allThreadsStopped` means only the named thread
 * stopped, absent `allThreadsContinued` means ALL threads continued.
 */

/* Bounds, shared with the UI subplan, which renders exactly these.  Every
 * one of them is an adapter-controlled collection, so every one of them has
 * a number rather than a hope. */
#define DAP_EXEC_MAX_THREADS 256
#define DAP_EXEC_MAX_FRAMES 200
#define DAP_EXEC_MAX_SCOPES 64
#define DAP_EXEC_MAX_VARIABLES 4096
#define DAP_EXEC_MAX_DEPTH 32
/* One `stackTrace` page.  The first request of a stop asks for ONE frame
 * (both v1 adapters advertise `supportsDelayedStackTraceLoading`); this is
 * what a second request asks for when frame zero cannot be navigated to, or
 * when the user walks the stack. */
#define DAP_EXEC_STACK_PAGE 20
/* Three `thread` events in 70 ms were measured from one `t.start()`, and
 * dape's own note is that some adapters blow up unless the re-request is
 * throttled.  This is the quiet period one event opens. */
#define DAP_EXEC_THREAD_DEBOUNCE_MS 100u

/* One line of adapter text as kg keeps it, bounded like every other. */
#define DAP_EXEC_TEXT_MAX 160
#define DAP_EXEC_NAME_MAX 128

struct kg_json_value; /* src/json.h */

/* What the user asked the program to do.  One enum rather than five entry
 * points because the refusal, the epoch bump, the RESUMING transition and
 * the synthesised `continued` are the same four steps for all of them, and
 * only the wire command and the granularity differ. */
enum dap_exec_step {
	DAP_EXEC_CONTINUE = 0,
	DAP_EXEC_NEXT,
	DAP_EXEC_STEP_IN,
	DAP_EXEC_STEP_OUT,
	/* `next` with `granularity:"instruction"`, sent only when the
	 * adapter advertised `supportsSteppingGranularity` -- an adapter
	 * that did not is asked for a plain `next` rather than for a member
	 * it may refuse the whole request over. */
	DAP_EXEC_STEP_INSTRUCTION,
};

struct dap_exec_thread {
	int id;
	char name[DAP_EXEC_NAME_MAX];
	bool stopped;
};

/* A frame, copied out.  `navigable` is the whole of [M-11]'s source
 * lesson: the very first lldb-dap stack carries a real path, a debug-info
 * relative path that does not exist, and a synthetic frame with a non-zero
 * `sourceReference` -- so a frame is navigable only when it names an
 * ABSOLUTE path that exists as a regular file, and everything else is
 * listed and left alone rather than handed to file visiting. */
struct dap_exec_frame {
	int id;
	int line;
	int column;
	bool navigable;
	char name[DAP_EXEC_NAME_MAX];
	char *path; /* owned; "" when the frame named none */
};

struct dap_exec_scope {
	char name[DAP_EXEC_NAME_MAX];
	int variables_reference;
	bool expensive;
};

/* One variable node.  `parent` is the index of the variable it was expanded
 * from, or -1 for a scope's own child, and it is what the cycle check walks:
 * an adapter that hands back a `variablesReference` already in a node's own
 * ancestry is describing a cycle, and kg stops rather than following it. */
struct dap_exec_variable {
	char name[DAP_EXEC_NAME_MAX];
	char value[DAP_EXEC_TEXT_MAX];
	char type[DAP_EXEC_NAME_MAX];
	int variables_reference;
	int scope;
	int parent;
	int depth;
};

/* Where this module's news goes.  One `ctx` for all of them; any may be
 * NULL, and a NULL hook drops what it would have been told -- which is what
 * a test binary with no editor in it wants.
 *
 * `location` is the stop's source line.  `focus` is false when the stop
 * carried `preserveFocusHint`: the panes and the decorations still move,
 * the user's window does not.  It is called only for a frame kg decided is
 * navigable, so an implementation never has to re-check the path.
 *
 * `relaunch` is `dap-restart` against an adapter that does not advertise
 * the `restart` request: this module has ended the old session and the
 * caller starts a new one from the configuration it remembered.  Called
 * once per restart, from the top level and never from inside a response
 * callback. */
struct dap_exec_hooks {
	void *ctx;
	void (*location)(void *ctx, const char *path, int line, bool focus);
	void (*report)(void *ctx, bool error, const char *text);
	void (*changed)(void *ctx);
	void (*relaunch)(void *ctx);
};

void dap_exec_set_hooks(const struct dap_exec_hooks *hooks);

/* Every event the session forwarded.  Has struct dap_session_hooks::event's
 * signature exactly, so the command layer installs it as the session's
 * event hook and nothing stands between an adapter's news and the model.
 *
 * `stopped`, `continued` and `thread` are the model's own; `breakpoint` is
 * routed to the table (src/dap_breakpoint.h), which is where stage 5 said
 * stage 6 would put it; everything else is ignored after this returns. */
void dap_exec_event(
    void *ctx, const char *event, const struct kg_json_value *body);

/* A session began, or ended.  Ending bumps the epoch [M-4] and drops the
 * whole model: a frame id from a dead adapter is not a frame id. */
void dap_exec_session_started(void);
void dap_exec_session_ended(void);

/* The debounced `thread` re-request and nothing else; called from
 * dap_poll().  Nonzero means a repaint is wanted, dap_poll()'s convention. */
int dap_exec_poll(void);

/* THE ONE ORACLE.  Every execution command asks this and no command forms
 * its own opinion, so "there is nothing stopped to step" is one rule with
 * one sentence (dap_exec_no_thread_text()).  True fills `*out` with the
 * thread the commands would act on. */
bool dap_exec_stopped_thread(int *out);
const char *dap_exec_no_thread_text(void);

/* Let the program run.  False when the oracle refused, having reported why.
 * On true the epoch has already bumped, the session is RESUMING, and a
 * local `continued` is synthesised when the adapter answers -- a real one
 * may arrive before that response (delve), after it (lldb-dap), or never
 * [M-10], and the handler is idempotent for all three. */
bool dap_exec_resume(enum dap_exec_step step);

/* Ask a running program to stop.  The mirror of the oracle: refused when
 * there is no session, accepted whenever one is running. */
bool dap_exec_pause(void);

/* `dap-goto`, in the two halves the protocol has, both gated on
 * `supportsGotoTargetsRequest`.  The first asks `gotoTargets` for
 * (source, line); when it answers exactly one target the `goto` follows
 * immediately.  When it answers several, they are retained -- tagged with
 * the selection generation, so a stack walk invalidates them -- and the
 * caller is told to choose: dap_exec_goto_target_count()/_target_name()
 * are that list and dap_exec_goto_chosen() sends the `goto`.
 *
 * The choice is a second command rather than a prompt from inside the
 * response callback on purpose: that callback runs from the idle poll,
 * which may be underneath a minibuffer prompt already (src/compile.h says
 * the same thing about programmatic compilations). */
bool dap_exec_goto(const char *path, int line);
size_t dap_exec_goto_target_count(void);
const char *dap_exec_goto_target_name(size_t index);
bool dap_exec_goto_chosen(size_t index);

/* `dap-restart`.  The adapter's own `restart` request when it advertised
 * one -- the spec's stated preference, and lldb-dap advertises it through
 * the capabilities event rather than at initialize -- and otherwise the
 * session is ended and the `relaunch` hook is called, which is the
 * client-side terminate-and-relaunch debugpy and nbcode need.  The
 * breakpoint table and the window layout are editor property and survive
 * either road untouched. */
bool dap_exec_restart(void);

/* Evaluate `expression` against the selected frame and report the answer.
 * `context` is the protocol's, and it matters: on lldb-dap `"repl"` returns
 * an LLDB-formatted `"(int) $0 = 21551"` with a `$0` side effect while
 * `"hover"` returns a bare `"32767"` [M-14].  A failed evaluate is reported
 * and is not a session error. */
bool dap_exec_evaluate(const char *expression, const char *context);

/* Walk the stack within one stop.  `delta` is -1 for "towards the caller"
 * and +1 for "towards the callee".  The first walk of a stop pages the
 * stack in, since the stop itself fetched one frame; the selection is
 * applied when the page lands. */
bool dap_exec_frame_select(int delta);

/* ------------------------------ what is known ------------------------- */

unsigned dap_exec_epoch(void);
unsigned dap_exec_selection(void);
const char *dap_exec_stop_reason(void);
int dap_exec_selected_thread(void);

size_t dap_exec_thread_count(void);
bool dap_exec_thread(size_t index, struct dap_exec_thread *out);
size_t dap_exec_frame_count(void);
/* `out->path` points into this module and is valid until the next poll. */
bool dap_exec_frame(size_t index, struct dap_exec_frame *out);
size_t dap_exec_selected_frame(void);
size_t dap_exec_scope_count(void);
bool dap_exec_scope(size_t index, struct dap_exec_scope *out);
size_t dap_exec_variable_count(void);
bool dap_exec_variable(size_t index, struct dap_exec_variable *out);

/* Expand one variable node's children.  The UI subplan's seam, and the one
 * entry point the ancestry cycle check exists for; refused for a node with
 * no reference, past the depth bound, or whose reference already appears in
 * its own ancestry. */
bool dap_exec_expand_variable(size_t index);

/* Where the program is stopped, for the decoration projection: the selected
 * frame's source line when it has a navigable one.  False when nothing is
 * stopped or the selected frame names no file kg can open. */
bool dap_exec_current_location(char *path, size_t path_size, int *line);

#endif /* KG_DAP_EXEC_H */
