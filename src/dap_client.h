#ifndef KG_DAP_CLIENT_H
#define KG_DAP_CLIENT_H

#include <stddef.h>

/* One debug adapter, spoken to as the Debug Adapter Protocol: the two
 * message-number spaces, the pending table its responses are matched in, the
 * events it sends unasked, the reverse requests it asks back, and the
 * mutable capability set every later gate reads.
 *
 * This is the protocol brain and it holds no editor state: no buffer, no
 * window, no command, no path.  It owns a transport
 * (src/dap_transport.h), the JSON layer parses and builds its messages, and
 * that is the whole of what it depends on -- which is what lets a unit test
 * and a fuzz harness link it with neither an editor nor a session.  The
 * session that decides WHAT to ask (doc/plans/dap/01-protocol.md stage 4)
 * sits above it and is the only caller with an opinion about debugging.
 *
 * Nothing here blocks and nothing here sleeps.  Every call queues work and
 * returns; dap_client_poll(), from the editor's poll sites, is what moves
 * bytes, dispatches messages and runs callbacks.
 *
 * THE TWO SEQ SPACES [M-5].  Each side of a DAP conversation numbers its own
 * messages, and the adapter's numbering may be worthless: lldb-dap stamps
 * `seq: 0` on every response and every event it sends.  So kg keeps one
 * monotonic counter, starting at 1 and never emitting zero, stamped on the
 * requests it sends AND on the responses it sends back to reverse requests;
 * it matches responses by `request_seq` and by nothing else; it tolerates
 * any inbound `seq`, including zero and including netcoredbg's string
 * spelling, and it echoes a reverse request's `seq` back verbatim -- a zero
 * included -- because that number is the adapter's to recognise and not
 * kg's to improve.  Running the counter out (int32) fails the session rather
 * than wrapping onto a number somebody is still waiting for.
 *
 * THE PENDING TABLE is src/lsp_client.c's proven shape, and the invariant is
 * the same one: a registered callback runs EXACTLY ONCE, whatever happens.
 * The answer runs it; if no answer will ever come -- the deadline passed,
 * the adapter died, the table was full, the send failed, the session was
 * closed -- the failure runs it instead, which is what keeps a caller's
 * context from leaking.  The slot is copied out and cleared before its
 * callback runs, so a callback may ask a new question from inside the
 * failure path.
 *
 * BOUNDED DISPATCH.  A message is parsed off the transport's borrowed buffer
 * into a document that owns its own bytes, dispatched, and released, one at
 * a time: kg never queues inbound messages, so there is no drain-then-
 * dispatch queue for a peer to fill.  What is bounded instead is the WORK
 * one poll may do (DAP_CLIENT_MAX_MESSAGES_PER_POLL,
 * DAP_CLIENT_MAX_BYTES_PER_POLL), so an adapter flooding events cannot
 * starve terminal input; dap_client_wants_poll() then says the budget ran
 * out and the caller should come back rather than wait for an idle tick.
 * Handlers may send and flush -- answering a reverse request is a write --
 * but they never re-enter receive or dispatch: that is the jsonrpc.el
 * reentrancy bug class, and dap_client_poll() refuses it.
 */

#define DAP_CLIENT_MAX_PENDING 64
/* One poll's work budget.  The message count is the one that matters for
 * responsiveness -- 64 messages is far more than any stop produces and far
 * less than a flood -- and the byte budget is what stops 64 large ones from
 * being just as bad.  The first message of a poll is always dispatched
 * however large it is, so a single legal oversized body (a deep `variables`
 * reply) is never stuck behind its own size. */
#define DAP_CLIENT_MAX_MESSAGES_PER_POLL 64
#define DAP_CLIENT_MAX_BYTES_PER_POLL (4u * 1024u * 1024u)
/* The longest command name stored per pending request.  The protocol's
 * longest is `setExceptionBreakpoints` at 24; an adapter naming something
 * longer gets its request refused rather than silently truncated into a
 * name that would then never match. */
#define DAP_CLIENT_COMMAND_MAX 32
/* One line of adapter-supplied text as kg keeps it: bounded, one line, and
 * with the C0 bytes replaced by spaces (see struct dap_error). */
#define DAP_CLIENT_TEXT_MAX 160
#define DAP_CLIENT_MAX_EXCEPTION_FILTERS 32
/* Output events that arrive before anything exists to show them in.  Both
 * numbers are bounds on an adapter's doing, so both are small: what lands
 * here in practice is debugpy's few pre-initialize bytes, and an adapter
 * that writes more before kg has a destination is one whose output is worth
 * less than the memory to hold it. */
#define DAP_CLIENT_HELD_OUTPUT_CHUNKS 8
#define DAP_CLIENT_HELD_OUTPUT_BYTES 256

struct dap_client;
struct dap_transport; /* src/dap_transport.h; owned, never inspected here */
struct kg_json_value; /* src/json.h */

/* Why a request failed, in the words a user reads [M-6].  Adapters put the
 * reason in two different places -- debugpy in the response's `message`,
 * lldb-dap only in `body.error.format` -- so both are read, in that order,
 * and a response that carries neither gets a sentence kg synthesises naming
 * the command.  `message` is therefore never empty.
 *
 * The text is a COPY rather than a borrowed node, so a caller may keep the
 * struct past the callback; it is bounded to one echo-area line; and every
 * C0 byte in it is a space, because a message kg puts in front of a user is
 * one line of text and an adapter does not get to spell a newline, a
 * carriage return or an escape into it.  What makes the bytes safe to
 * PAINT is still display_glyph_at() at the other end, as for every other
 * byte kg reads.
 *
 * `url`/`url_label` are empty when the adapter sent none, and `show_user`
 * is the adapter's own opinion about whether this is worth showing at all
 * -- kept because the two adapters disagree about which failures are the
 * user's business, and dropping it would make that decision here. */
struct dap_error {
	char message[DAP_CLIENT_TEXT_MAX];
	char url[DAP_CLIENT_TEXT_MAX];
	char url_label[64];
	bool show_user;
};

/* An answer, or the failure standing in for one.
 *
 * `success` false is DATA: an adapter that refused a request is a healthy
 * adapter, and `error` then says why.  `answered` is the different
 * question -- whether the adapter replied at all -- and it is false only
 * for the failures kg synthesises when no reply will ever arrive.  So a
 * callback that only wants to know "did I get what I asked for" tests
 * `success`, and one that has to tell a refusal from a death tests
 * `answered` as well.
 *
 * `body` is the response's own `body` member, borrowed from the parsed
 * message and valid only for the duration of the call: copy anything that
 * outlives it.  It is NULL when the response carried none and always NULL
 * for a synthesised failure. */
struct dap_response {
	const char *command;
	bool success;
	bool answered;
	const struct kg_json_value *body;
	const struct dap_error *error; /* NULL when `success` */
};

typedef void (*dap_response_fn)(
    struct dap_client *c, const struct dap_response *r, void *ctx);

/* Every event the adapter sent that kg did not have to handle itself:
 * `body` is the event's own body, borrowed for the duration of the call and
 * NULL when it carried none.
 *
 * Two events never reach here.  `output` goes to the output hook below,
 * because its destination is a transcript rather than a state machine, and
 * an `output` with category `telemetry` goes nowhere at all [M-12].  Event
 * names kg does not know are delivered like any other: which events matter
 * is a decision for the layer that knows what a thread is, and this one
 * takes no view. */
typedef void (*dap_event_fn)(
    void *ctx, const char *event, const struct kg_json_value *body);

/* Bytes the debuggee or the adapter wrote, with the protocol's category
 * (`stdout`, `stderr`, `console`, ...).  `text` is counted rather than
 * NUL-terminated, and is bytes rather than a line: debugpy splits one
 * print across two events mid-line, so a consumer appends and never assumes
 * an event is a line [M-12]. */
typedef void (*dap_output_fn)(
    void *ctx, const char *category, const char *text, size_t len);

/* What went wrong, or what was ignored.  DEBUG is the unknown-and-ignored
 * traffic the protocol says to tolerate -- an event kg does not implement, a
 * response nobody was waiting for [M-13] -- and ERROR is an adapter breaking
 * the envelope rules.  Neither ends a session by itself. */
enum dap_log_level {
	DAP_LOG_DEBUG = 0,
	DAP_LOG_ERROR,
};

typedef void (*dap_log_fn)(
    void *ctx, enum dap_log_level level, const char *text);

/* Where a client's unsolicited traffic goes.  One `ctx` for all three,
 * because one thing owns a session; any of them may be NULL, and a NULL
 * hook drops what it would have been told -- which is what a test binary
 * with no editor in it wants, and what the fuzz harness's null UI is. */
struct dap_client_hooks {
	void *ctx;
	dap_event_fn event;
	dap_output_fn output;
	dap_log_fn log;
};

/* The capabilities kg gates on.  Each row exists because a later stage
 * branches on it (doc/plans/dap/01-protocol.md stages 4-6): a capability
 * nothing reads would be a field to keep in step for no one.  The wire
 * spellings, including debugpy's misspelling of one of them, live in
 * src/dap_client.c beside the merge. */
enum dap_capability {
	DAP_CAP_CONFIGURATION_DONE = 0,
	DAP_CAP_CONDITIONAL_BREAKPOINTS,
	DAP_CAP_HIT_CONDITIONAL_BREAKPOINTS,
	DAP_CAP_LOG_POINTS,
	DAP_CAP_EVALUATE_FOR_HOVERS,
	DAP_CAP_SET_VARIABLE,
	DAP_CAP_TERMINATE_REQUEST,
	DAP_CAP_TERMINATE_DEBUGGEE,
	DAP_CAP_RESTART_REQUEST,
	DAP_CAP_GOTO_TARGETS,
	DAP_CAP_STEP_IN_TARGETS,
	DAP_CAP_STEPPING_GRANULARITY,
	DAP_CAP_DELAYED_STACK_TRACE_LOADING,
	DAP_CAP__COUNT
};

/* One entry of `exceptionBreakpointFilters`, copied rather than borrowed
 * because it outlives the message it arrived in and is read again at every
 * configuration.  `default_on` is the filter's own `default`, which is what
 * gives Python uncaught-exception stops for free [M-7]. */
struct dap_exception_filter {
	char filter[DAP_CLIENT_COMMAND_MAX];
	char label[64];
	char description[DAP_CLIENT_TEXT_MAX];
	bool default_on;
	bool supports_condition;
};

/* Take over `t` and speak the protocol on it.  The client owns the
 * transport from here: dap_client_close() closes it, and a caller that
 * still wants to drive the end-of-session ladder asks for it back with
 * dap_client_transport().  Returns NULL, having closed `t`, when there is
 * no memory for the client. */
struct dap_client *dap_client_new(struct dap_transport *t);

/* Fail everything outstanding, close the transport and free the client.
 * Every pending callback runs once, with no answer, before anything is
 * freed -- this is also the cancel path, since cancelling a start is
 * closing the transport under it.  NULL is accepted and ignored. */
void dap_client_close(struct dap_client *c);

/* Install the hooks above.  Any output that arrived before a destination
 * existed is handed to a new output hook here, in order, bounded by
 * DAP_CLIENT_HELD_OUTPUT_*; NULL clears them all. */
void dap_client_set_hooks(
    struct dap_client *c, const struct dap_client_hooks *hooks);

/* Whether this adapter's own standard error carries the DEBUGGEE's output.
 * Off by default, which is every adapter but one: for the others that
 * channel is the adapter complaining about itself and belongs in the log.
 * When on, the bytes reach the output hook on the `stderr` category
 * UNSPLIT -- never cut into lines, since a newline invented at a chunk
 * boundary is kg rewriting what the program printed -- and the log hook
 * stops seeing them, so nothing is delivered twice.
 *
 * The client does not decide this and does not know which adapter it is
 * talking to: it is a column of the adapter spec, and delve is the one row
 * that sets it (doc/plans/dap/04-go.md, measured). */
void dap_client_set_adapter_stderr_to_output(struct dap_client *c, bool on);

/* What kg ran to get this adapter, for the one failure that has to name it:
 * a spawned adapter that never announced the port it was listening on.  The
 * text is copied and bounded, and nothing else ever reads it. */
void dap_client_set_adapter_command(struct dap_client *c, const char *command);

/* Replace the two deadlines the client enforces (milliseconds), for a test
 * that would otherwise wait one out.  Zero switches a deadline off.  It
 * cannot switch one ON for launch or attach, which are exempt by protocol
 * rather than by policy [M-1]. */
void dap_client_set_timeouts(
    struct dap_client *c, unsigned initialize_ms, unsigned request_ms);

/* Send `initialize` with the payload the parent plan measured sufficient
 * for every v1 adapter and nothing more -- every flag kg does not send is a
 * promise it does not make, and it deliberately advertises neither
 * `supportsRunInTerminalRequest` nor `supportsStartDebuggingRequest`
 * [M-2, M-3].  `adapter_id` is the adapter's own `adapterID`.
 *
 * The capabilities in the reply are merged before `cb` runs, so a callback
 * that sends `launch` from here already sees dap_capable()'s live answers;
 * an `initialized` event that arrived early is delivered after it. */
long long dap_client_initialize(struct dap_client *c, const char *adapter_id,
    dap_response_fn cb, void *ctx);

/* Ask the adapter something.  `arguments`/`arguments_len` are a pre-encoded
 * JSON value the caller built and still owns, or NULL for a request with no
 * arguments.
 *
 * Returns the message number the request went out as (positive), or -1.
 * Unlike src/lsp_client.h's, a -1 here does NOT mean the callback will
 * never run: a request that cannot be sent -- a dead client, a full pending
 * table, a transport that refused the bytes -- fails its callback LOCALLY
 * and SYNCHRONOUSLY, before this returns, so the caller's context is
 * released on this path as on every other.  Exactly once, either way.
 *
 * Every request carries a deadline except `launch` and `attach`, which have
 * none [M-1]: their response has no fixed position, a debuggee may take as
 * long as it likes to start, and what replaces the deadline is cancellation
 * -- closing the transport, which fails the request like any other death. */
long long dap_client_request(struct dap_client *c, const char *command,
    const char *arguments, size_t arguments_len, dap_response_fn cb, void *ctx);

/* Service the adapter: flush what is queued, dispatch what has arrived,
 * hand its standard error to the log hook a line at a time, abandon
 * whatever request has run out of time, and notice a transport that died.
 * Returns nonzero when something a user could see changed, which is
 * lsp_client_poll()'s repaint convention.
 *
 * Called from inside a handler it does nothing (and asserts): dispatch is
 * not re-entrant, and the follow-up poll below is how the work still gets
 * done. */
int dap_client_poll(struct dap_client *c);

/* Whether the last poll stopped on its work budget rather than because the
 * adapter had gone quiet, so the caller should poll again promptly instead
 * of waiting for its next idle tick. */
bool dap_client_wants_poll(const struct dap_client *c);

/* The descriptors a caller's own wait should include so the next poll
 * happens when the adapter writes rather than when a clock says so; the
 * transport decides which, and DAP_TRANSPORT_WAIT_FDS_MAX is what to size
 * `fds` against. */
int dap_client_wait_fds(const struct dap_client *c, int *fds, int max);

/* Whether the conversation is still usable.  A client stops being usable
 * for exactly one reason at a time and keeps the first: the transport
 * failed or ended, the adapter sent something that is not JSON, the message
 * numbers ran out, or it was closed.  There is no repair -- a session
 * starts a new adapter -- and dap_client_death_text() is the sentence a
 * user is told, "" while it is alive. */
bool dap_client_alive(const struct dap_client *c);
const char *dap_client_death_text(const struct dap_client *c);

/* The live capability set: merged member by member from the `initialize`
 * response and from every `capabilities` event, so a gate always reads what
 * the adapter says NOW rather than what it said first [M-7]. */
bool dap_capable(const struct dap_client *c, enum dap_capability cap);

/* The adapter's exception filters, as of the last capability set that
 * supplied any.  An adapter that sends a `capabilities` event without them
 * keeps the ones it had. */
size_t dap_client_exception_filter_count(const struct dap_client *c);
const struct dap_exception_filter *dap_client_exception_filter(
    const struct dap_client *c, size_t index);

/* Whether the `initialized` event has been seen.  It is a bool rather than
 * an event the session might miss because adapters send it before the
 * `initialize` response [M-13]: kg remembers that one bit, delivers the
 * event once capabilities are settled, and the session's configuration
 * milestone reads the bit rather than racing the delivery. */
bool dap_client_initialized_seen(const struct dap_client *c);

/* How many requests are outstanding, and whether the answer to a -1 from
 * dap_client_request() is "kg is full" rather than "the adapter is gone".
 * For tests and for a caller that has to say which it was. */
size_t dap_client_pending_count(const struct dap_client *c);

/* The transport, still owned by the client: the session drives the
 * end-of-session ladder (dap_transport_begin_shutdown()) on it, and C-g
 * cancels a launch by closing it.  NULL is never returned for a live
 * client. */
struct dap_transport *dap_client_transport(struct dap_client *c);

#endif /* KG_DAP_CLIENT_H */
