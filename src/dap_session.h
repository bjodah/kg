#ifndef KG_DAP_SESSION_H
#define KG_DAP_SESSION_H

#include "dap_client.h"
#include "dap_config.h"

#include <limits.h>
#include <stddef.h>

/* One debugging session: the choreography that turns an adapter into a
 * stopped program, and the lattice that ends it again.
 *
 * The client below (src/dap_client.h) knows how to ask an adapter
 * something; this layer knows WHAT to ask and WHEN, which is the part every
 * adapter disagrees about.  It holds no editor state either -- no buffer,
 * no window, no command -- so a unit test drives a real session against a
 * real adapter with no editor linked, and stages 5 and 6 hang the
 * breakpoint table and the commands off the seams below.
 *
 * WHY A PHASE PLUS MILESTONES, AND NOT ONE ENUM.  The launch response has
 * no fixed position [M-1]: lldb-dap answers `launch` BEFORE the
 * `initialized` event, debugpy answers it AFTER the `configurationDone`
 * response, and nbcode and delve answer between the two.  Four adapters,
 * three orderings, and no linear state machine that fits all of them.  So
 * the session carries a coarse phase for "what can the user do now" and
 * explicit milestone bools for "what has happened", and the transitions are
 * idempotent FUNCTIONS rather than assignments scattered through callbacks:
 * becoming active is reachable from either the launch response or the
 * configurationDone response, and reaching it twice is normal.
 *
 * A client that waited for the launch response before configuring would
 * deadlock debugpy; one that required `initialized` first would
 * mis-sequence lldb-dap.  What kg does instead is exactly dape's: send
 * launch or attach from the initialize response handler, with NO deadline
 * [M-1] and cancellation instead, and drive configuration off the
 * `initialized` event alone.
 *
 * TEARDOWN IS A MATRIX, NOT A VERB [M-8].  `exited` reports the debuggee's
 * exit code and does NOT end the session; `terminated` does.  What kg sends
 * to end one depends on why it is ending and on whether the session
 * LAUNCHED the debuggee or ATTACHED to it -- detaching is the point of
 * `dap-disconnect`, and an attached debuggee must survive the editor
 * exiting.  dap_session_end_action() is that matrix as a pure function, so
 * the policy can be read, and tested, without a live adapter.
 *
 * ONE SESSION IN v1, but the struct carries `parent`/`children` and every
 * caller asks dap_session_current() rather than holding a pointer, so the
 * later change is confined to that oracle (parent plan, Architecture).
 */

/* One session, and the descriptors it can contribute to the editor's wait.
 * KG_DAP_WAIT_FDS_MAX (src/dap.h) is this times the transport's bound, and
 * src/dap_session.c asserts that at compile time. */
#define DAP_SESSION_MAX_SESSIONS 1

/* The configuration join's bounds: how many sources one `setBreakpoints`
 * round may cover and how many breakpoints one source may carry.  Both are
 * far past a working session and are what stop a runaway breakpoint table
 * from becoming a thousand requests at launch. */
#define DAP_SESSION_MAX_SOURCES 32
#define DAP_SESSION_MAX_SOURCE_LINES 64

/* One line of text the session hands a user, bounded for the echo area the
 * way the client's errors are. */
#define DAP_SESSION_TEXT_MAX 256

/* What the user can do, coarsely.  The milestones below say what has
 * happened; this says what the session IS.
 *
 * STOPPED is the only phase in which a thread can be inspected or stepped,
 * and it is reachable from STARTING: a `stopped` event can arrive before
 * the `configurationDone` response (lldb-dap with stopOnEntry, [M-10]), so
 * the stop handler must be safe while the session is nominally still
 * configuring.  ENDING is the teardown ladder running; DEAD is final and
 * every transition out of it is refused rather than asserted. */
enum dap_phase {
	DAP_PHASE_STARTING = 0,
	DAP_PHASE_ACTIVE,
	DAP_PHASE_STOPPED,
	DAP_PHASE_RESUMING,
	DAP_PHASE_ENDING,
	DAP_PHASE_DEAD,
};

/* Why a session is ending, which is half of what decides what kg sends. */
enum dap_end_request {
	/* The adapter said so: a `terminated` event. */
	DAP_END_NATURAL = 0,
	/* `dap-disconnect`: leave the debuggee running. */
	DAP_END_DISCONNECT,
	/* `dap-terminate`: the user asked for the debuggee to die. */
	DAP_END_TERMINATE,
	/* The editor is exiting, or otherwise cleaning up. */
	DAP_END_CLEANUP,
	/* The launch or attach was refused, or the handshake never
	 * completed: there is nothing to disconnect from politely. */
	DAP_END_LAUNCH_FAILURE,
};

/* What kg sends, if anything, before closing the transport. */
enum dap_end_action {
	/* Nothing: close the transport, whose own ladder half-closes, waits
	 * out the grace period and kills only a child it owns. */
	DAP_END_ACTION_CLOSE = 0,
	DAP_END_ACTION_DISCONNECT_KEEP, /* terminateDebuggee: false */
	DAP_END_ACTION_DISCONNECT_TERMINATE, /* terminateDebuggee: true */
	DAP_END_ACTION_TERMINATE, /* the `terminate` request */
};

/* The teardown matrix, as a pure function of the four things it depends on
 * [M-8].  The rows, in the plan's own order:
 *
 *   cause              | launched                  | attached
 *   -------------------+---------------------------+--------------------
 *   natural terminated | close the owned transport | same
 *   dap-disconnect     | disconnect, keep debuggee | same (the point)
 *   dap-terminate, cap | terminate, then close     | terminate
 *   dap-terminate, no  | disconnect, terminating   | same, and only
 *   cap but debuggee   |                           | because the user
 *                      |                           | asked to terminate
 *   editor exit        | terminate the debuggee    | detach, do not
 *   launch failure     | disconnect / close        | close the socket
 *
 * The one row not in the plan's table is what a `dap-terminate` does
 * against an adapter that advertises neither `terminate` nor
 * `terminateDebuggee`: there is no way to ask, so it detaches and says so
 * rather than pretending.
 *
 * A timed-out `terminate` is forceful user-requested termination, not a
 * protocol failure: the transport is closed, and its kill backstop is what
 * collects an owned child.  It never loops into a second attempt. */
enum dap_end_action dap_session_end_action(enum dap_end_request request,
    bool attached, bool can_terminate, bool can_terminate_debuggee);

/* One source's breakpoints, as the session asks for them at configuration
 * time.  Stage 5 owns the table this comes from; the session only needs
 * a path and lines, which is what keeps the two apart. */
struct dap_session_source {
	char path[PATH_MAX];
	int lines[DAP_SESSION_MAX_SOURCE_LINES];
	size_t line_count;
};

struct dap_session;

/* Where a session's news goes.  One `ctx` for all of them, because one
 * thing owns a session, and any hook may be NULL.
 *
 * `sources` is the breakpoint snapshot the configuration sequence sends:
 * it is asked ONCE, when configuration starts, and the answers are matched
 * against that snapshot rather than against whatever the table holds by the
 * time a response arrives.  A session with no provider configures an empty
 * source list, which is a legal debugging session with no breakpoints. */
struct dap_session_hooks {
	void *ctx;
	void (*changed)(void *ctx, struct dap_session *session);
	void (*report)(void *ctx, bool error, const char *text);
	dap_output_fn output;
	dap_event_fn event;
	size_t (*sources)(
	    void *ctx, struct dap_session_source *out, size_t max);
};

/* What to start, and how.  `arguments` are the expanded bytes
 * dap_config_expand_arguments() produced -- borrowed here and copied, so
 * the caller may free them as soon as this returns. */
struct dap_session_request {
	const struct dap_adapter_spec *adapter;
	enum dap_request_kind request;
	const char *arguments;
	size_t arguments_len;
	const char *config_name;
	const struct dap_session_hooks *hooks;
};

/* Everything a caller may read about a session, copied out rather than
 * exposed as a struct with pointers in it: a UI that painted from a live
 * session between two polls would be reading a state machine mid-step.
 *
 * The milestones are the plan's own, and every one of them is here because
 * something branches on it rather than because it was easy to record. */
struct dap_session_state {
	enum dap_phase phase;
	bool attached;
	bool initialize_done;
	bool launch_sent;
	bool launch_done;
	bool launch_failed;
	bool initialized_seen;
	bool configuration_started;
	bool configuration_done;
	/* Whether `configurationDone` was actually SENT, which is not the
	 * same question: without the capability the milestone completes
	 * after exception configuration and no request goes out. */
	bool configuration_done_sent;
	/* The join's tally: one failed source is reported and never wedges
	 * the rest. */
	size_t sources_sent;
	size_t sources_answered;
	size_t sources_failed;
	/* `exited` is status, not an ending. */
	bool exited;
	int exit_code;
	bool terminated_seen;
	/* `terminated` may carry a `restart` value; retaining it is what a
	 * restart policy later reads. */
	bool restart_requested;
	enum dap_end_request end_request;
	enum dap_end_action end_action;
	/* The thread the session is looking at, or 0 for none.  Stage 6
	 * fills the rest of the stop model in behind this one number. */
	int thread_id;
	char status[DAP_SESSION_TEXT_MAX];
};

/* Start an adapter and begin the handshake.  Returns NULL with `error`
 * filled in when the adapter could not be started at all; a session that
 * comes back is registered as the current one and is polled from there.
 *
 * Nothing blocks: `initialize` is queued, and everything after it happens
 * in dap_session_poll(). */
struct dap_session *dap_session_start(
    const struct dap_session_request *request, char *error, size_t error_size);

/* The one session, or NULL.  Every command asks this rather than holding a
 * pointer, which is the seam multi-session support will widen. */
struct dap_session *dap_session_current(void);

/* Service the session: the client, then the teardown ladder if one is
 * running.  Nonzero means a repaint is wanted, lsp_poll()'s convention. */
int dap_session_poll(struct dap_session *session);

/* The facade's legs (src/dap.h): every registered session, polled and
 * asked for its descriptors.  dap_session_shutdown_all() is the editor
 * exiting -- DAP_END_CLEANUP for each, which terminates a launched
 * debuggee and detaches an attached one -- bounded by `grace_ms` in total
 * and never blocking for more. */
int dap_session_poll_all(void);
int dap_session_wait_fds(int *fds, int max);
void dap_session_shutdown_all(unsigned grace_ms);

/* Begin ending the session, per the matrix above.  Idempotent: a session
 * already ending keeps the request it started with, so a second C-g or a
 * `terminated` arriving mid-teardown does not restart the ladder or send a
 * second request. */
void dap_session_end(struct dap_session *session, enum dap_end_request request);

/* C-g.  A launch has no deadline [M-1], so this is what ends one that will
 * never answer: the transport is closed under the conversation, every
 * pending callback fails exactly once, and the session is DEAD. */
void dap_session_cancel(struct dap_session *session);

/* Release the session.  Ends it forcefully if it has not ended, so this is
 * safe to call on a live one; the registry slot is cleared either way. */
void dap_session_close(struct dap_session *session);

void dap_session_get_state(
    const struct dap_session *session, struct dap_session_state *out);

/* The client, for the layers above that speak the protocol themselves
 * (stages 5 and 6: breakpoints and execution).  NULL once the session is
 * DEAD, which is also the one honest answer to "can I send something". */
struct dap_client *dap_session_client(struct dap_session *session);

/* The deadlines a test would otherwise wait out: the client's two
 * (initialize, ordinary request -- never launch, which has none by
 * protocol) and the transport's shutdown ladder. */
void dap_session_set_timeouts(
    struct dap_session *session, unsigned initialize_ms, unsigned request_ms);
void dap_session_set_shutdown_deadlines(
    struct dap_session *session, unsigned shutdown_ms, unsigned kill_ms);

#endif /* KG_DAP_SESSION_H */
