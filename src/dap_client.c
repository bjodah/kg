/* ===================== the Debug Adapter Protocol, spoken ================
 *
 * The protocol half of the debugger client: message numbers, the pending
 * table their answers land in, the events an adapter sends unasked, the
 * reverse requests it asks back, and the capability set every later gate
 * reads.  See src/dap_client.h for the contract and
 * doc/plans/dap/01-protocol.md stage 3 for where it sits.
 *
 * The shape is one transport and one pending table -- no outbound queue,
 * because unlike a language server an adapter is not entitled to ignore
 * what arrives before it has answered `initialize`; the ordering rule is
 * "nothing before the initialize response", and that is the session's to
 * keep rather than a FIFO's to enforce.
 *
 * Every incoming message is one of exactly three things and the envelope is
 * checked BEFORE anything acts on it, because each of the three has its own
 * required members and an adapter that omits one is not sending a message
 * with a missing field, it is sending something kg cannot correlate.  What
 * follows the check is short: a response finds its slot by `request_seq`, an
 * event goes to the hook that may care, a reverse request is refused
 * politely -- and silence is the one answer never given, since an adapter
 * waiting on a reply it will not get stops answering kg's own requests
 * [M-3].
 */

#include "dap_client.h"

#include "dap_transport.h"
#include "framed_io.h"
#include "json.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dap_held_output; /* defined below */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* One outstanding request.  `deadline_ms` is CLOCK_MONOTONIC milliseconds
 * after which nobody is waiting any more, and 0 is the launch/attach
 * exemption [M-1] -- and also what a client with its deadlines switched off
 * gives every request. */
struct dap_pending {
	int32_t seq;
	long long deadline_ms;
	dap_response_fn cb;
	void *ctx;
	bool used;
	char command[DAP_CLIENT_COMMAND_MAX];
};

/* Output that arrived with nowhere to put it.  Held rather than dropped
 * because the bytes an adapter writes before kg has a transcript are the
 * ones explaining why the session did not start; bounded in both directions
 * because an adapter that writes more than this before a destination exists
 * is not explaining anything. */
struct dap_held_output {
	char category[16];
	char text[DAP_CLIENT_HELD_OUTPUT_BYTES];
	size_t len;
};

struct dap_client {
	struct dap_transport *t;
	struct dap_client_hooks hooks;
	/* kg's own message numbers: monotonic, from 1, never zero [M-5]. */
	int32_t next_seq;
	unsigned initialize_ms;
	unsigned request_ms;
	bool dead;
	bool in_dispatch;
	bool poll_again;
	bool initialize_done;
	bool initialized_seen;
	/* An `initialized` event that beat the initialize response: one bool,
	 * not a pre-init event store [M-13]. */
	bool initialized_deferred;
	bool caps[DAP_CAP__COUNT];
	struct dap_exception_filter filters[DAP_CLIENT_MAX_EXCEPTION_FILTERS];
	size_t filter_count;
	struct dap_pending pending[DAP_CLIENT_MAX_PENDING];
	struct dap_held_output held[DAP_CLIENT_HELD_OUTPUT_CHUNKS];
	size_t held_count;
	size_t held_dropped;
	bool stderr_to_output;
	/* Wider than one message on purpose: the announce failure below
	 * names both a line and a command line, and the two together are
	 * longer than any other verdict this client produces. */
	char death[2 * DAP_CLIENT_TEXT_MAX];
	/* What kg ran to get this adapter, for the one message that has to
	 * name it, and the buffer that message is built in. */
	char command[DAP_CLIENT_TEXT_MAX];
	char death_detail[2 * DAP_CLIENT_TEXT_MAX];
};

/* ------------------------------- text --------------------------------- */

/* Copy adapter-supplied text into a bounded buffer as one line: every C0
 * byte and DEL becomes a space, so a message kg puts in front of a user
 * cannot carry a newline or an escape into it.  Bytes above 0x7F pass
 * through -- they are UTF-8 the renderer already knows how to measure --
 * and NULL becomes "".  What makes the bytes safe to PAINT is still
 * display_glyph_at(); this is what keeps them one line. */
static void safe_textn(const char *in, size_t len, char *out, size_t size)
{
	size_t i = 0;
	unsigned char ch;

	for (; in && i < len && i + 1 < size; i++) {
		ch = (unsigned char)in[i];
		out[i] = (ch < 0x20u || ch == 0x7fu) ? ' ' : (char)ch;
	}
	out[i] = '\0';
}

static void safe_text(const char *in, char *out, size_t size)
{
	safe_textn(in, in ? strlen(in) : 0, out, size);
}

static void client_log(
    struct dap_client *c, enum dap_log_level level, const char *text)
{
	if (c->hooks.log) {
		c->hooks.log(c->hooks.ctx, level, text);
	}
}

/* CLOCK_MONOTONIC in milliseconds: a deadline measured against a clock that
 * can be stepped is one an NTP correction can expire. */
static long long now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
	return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
}

/* -------------------------- protocol numbers -------------------------- */

/* A protocol id, range-checked into int32 [M-10]: the spec pins
 * `variablesReference` to (0, 2^31) and every id all four measured adapters
 * emitted fits, so a number outside it is an adapter fault to report rather
 * than a reason for a wider type.  A string of digits is coerced, because
 * netcoredbg spells `seq` that way and one strtoll is cheaper than a
 * conversation that cannot be correlated. */
static bool id_int32(const struct kg_json_value *v, int32_t *out)
{
	const char *s;
	char *end;
	long long n;

	if (kg_json_kind_of(v) == KG_JSON_NUMBER) {
		n = kg_json_int(v, LLONG_MIN);
	} else if ((s = kg_json_str(v, NULL)) != NULL && *s) {
		errno = 0;
		n = strtoll(s, &end, 10);
		if (errno != 0 || *end) {
			return false;
		}
	} else {
		return false;
	}
	if (n < INT32_MIN || n > INT32_MAX) {
		return false;
	}
	*out = (int32_t)n;
	return true;
}

static void client_die(struct dap_client *c, const char *why);

/* The next number kg stamps on a message.  Exhaustion fails the session
 * rather than wrapping onto a number a pending request still answers to --
 * unreachable in practice at one message per interaction, and one line to
 * state rather than a wrap to reason about. */
static bool seq_next(struct dap_client *c, int32_t *out)
{
	if (c->next_seq >= INT32_MAX) {
		client_die(c, "the debug session ran out of message numbers");
		return false;
	}
	*out = c->next_seq++;
	return true;
}

/* ---------------------------- message building ------------------------ */

static char *build_request(int32_t seq, const char *command,
    const char *arguments, size_t arguments_len, size_t *out_len)
{
	struct kg_jsonw w;
	char *text = NULL;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "seq");
	kg_jsonw_int(&w, seq);
	kg_jsonw_key(&w, "type");
	kg_jsonw_string(&w, "request");
	kg_jsonw_key(&w, "command");
	kg_jsonw_string(&w, command);
	if (arguments && arguments_len > 0) {
		kg_jsonw_key(&w, "arguments");
		kg_jsonw_raw(&w, arguments, arguments_len);
	}
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &text, out_len) != 0) {
		return NULL;
	}
	return text;
}

/* kg's answer to a reverse request.  `request_seq` is the adapter's own
 * number echoed verbatim -- a zero included, since lldb-dap stamps zero on
 * everything and it is that adapter's business to recognise its own
 * numbering [M-5] -- while `seq` comes from kg's counter, because the two
 * spaces are independent. */
static char *build_refusal(int32_t seq, int32_t request_seq,
    const char *command, const char *message, size_t *out_len)
{
	struct kg_jsonw w;
	char *text = NULL;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "seq");
	kg_jsonw_int(&w, seq);
	kg_jsonw_key(&w, "type");
	kg_jsonw_string(&w, "response");
	kg_jsonw_key(&w, "request_seq");
	kg_jsonw_int(&w, request_seq);
	kg_jsonw_key(&w, "success");
	kg_jsonw_bool(&w, false);
	kg_jsonw_key(&w, "command");
	kg_jsonw_string(&w, command);
	kg_jsonw_key(&w, "message");
	kg_jsonw_string(&w, message);
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &text, out_len) != 0) {
		return NULL;
	}
	return text;
}

/* The honest initialize payload (parent plan): measured sufficient for both
 * v1 adapters, and every flag not here is a promise kg does not make -- no
 * paging, no progress, no ANSI, and above all no reverse requests
 * [M-2, M-3]. */
static char *build_initialize_args(const char *adapter_id, size_t *out_len)
{
	struct kg_jsonw w;
	char *text = NULL;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "clientID");
	kg_jsonw_string(&w, "kg");
	kg_jsonw_key(&w, "clientName");
	kg_jsonw_string(&w, "kg");
	kg_jsonw_key(&w, "adapterID");
	kg_jsonw_string(&w, adapter_id && *adapter_id ? adapter_id : "kg");
	kg_jsonw_key(&w, "locale");
	kg_jsonw_string(&w, "en-US");
	kg_jsonw_key(&w, "linesStartAt1");
	kg_jsonw_bool(&w, true);
	kg_jsonw_key(&w, "columnsStartAt1");
	kg_jsonw_bool(&w, true);
	kg_jsonw_key(&w, "pathFormat");
	kg_jsonw_string(&w, "path");
	kg_jsonw_key(&w, "supportsVariableType");
	kg_jsonw_bool(&w, true);
	kg_jsonw_end_object(&w);
	if (kg_jsonw_finish(&w, &text, out_len) != 0) {
		return NULL;
	}
	return text;
}

/* ------------------------------- pending ------------------------------ */

static struct dap_pending *pending_alloc(struct dap_client *c)
{
	size_t i;

	for (i = 0; i < DAP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used) {
			return &c->pending[i];
		}
	}
	return NULL;
}

/* The failure a caller sees when no answer will ever arrive: not answered,
 * not successful, and a sentence saying which death it was. */
static void fail_callback(
    struct dap_client *c, const struct dap_pending *slot, const char *why)
{
	struct dap_error err;
	struct dap_response r;

	if (!slot->cb) {
		return;
	}
	memset(&err, 0, sizeof(err));
	memset(&r, 0, sizeof(r));
	safe_text(why, err.message, sizeof(err.message));
	r.command = slot->command;
	r.error = &err;
	slot->cb(c, &r, slot->ctx);
}

/* Fail everything outstanding.  The slot is copied out and cleared before
 * its callback runs, so a callback that asks a new question from inside the
 * failure path cannot be handed the slot it is standing in. */
static void pending_fail_all(struct dap_client *c, const char *why)
{
	struct dap_pending slot;
	size_t i;

	for (i = 0; i < DAP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used) {
			continue;
		}
		slot = c->pending[i];
		memset(&c->pending[i], 0, sizeof(c->pending[i]));
		fail_callback(c, &slot, why);
	}
}

/* Which slot answers to `seq`, or DAP_CLIENT_MAX_PENDING for none. */
static size_t pending_find(const struct dap_client *c, int32_t seq)
{
	size_t i;

	for (i = 0; i < DAP_CLIENT_MAX_PENDING; i++) {
		if (c->pending[i].used && c->pending[i].seq == seq) {
			return i;
		}
	}
	return DAP_CLIENT_MAX_PENDING;
}

/* Abandon one request whose deadline passed.  The adapter is left alone: it
 * is alive and owes an answer nobody is waiting for any more, and a reply
 * that turns up later finds no slot and is dropped like any other stray. */
static void pending_expire_one(struct dap_client *c, size_t index)
{
	struct dap_pending slot = c->pending[index];
	char text[DAP_CLIENT_COMMAND_MAX + 48];

	memset(&c->pending[index], 0, sizeof(c->pending[index]));
	snprintf(text, sizeof(text), "the adapter did not answer %s in time",
	    slot.command);
	client_log(c, DAP_LOG_ERROR, text);
	fail_callback(c, &slot, text);
}

static int pending_expire(struct dap_client *c)
{
	long long now = now_ms();
	int fired = 0;
	size_t i;

	for (i = 0; i < DAP_CLIENT_MAX_PENDING; i++) {
		if (!c->pending[i].used || c->pending[i].deadline_ms == 0
		    || c->pending[i].deadline_ms > now) {
			continue;
		}
		pending_expire_one(c, i);
		fired = 1;
	}
	return fired;
}

/* --------------------------------- death ------------------------------ */

/* What ended this transport, in the words a user reads.  framed_io already
 * knows which failure it was and, for the two that have sides, which side
 * (src/dap_transport.h); this is that verdict spelled out.  End of stream
 * is a death here like any other -- what makes it safe is that the session
 * never WAITS for it, since debugpy's adapter answers `disconnect` and
 * lives on. */
static const char *transport_death_text(struct dap_client *c)
{
	const char *prefix = dap_transport_announce_prefix(c->t);

	/* An adapter kg spawned that never announced its port failed at a
	 * step none of the framing verdicts below describes: there was never
	 * a connection for the framing to be wrong about.  So this message
	 * names the line kg was waiting for and the command it waited on,
	 * because the usual cause is an adapter configured to log somewhere
	 * other than its standard output -- delve's `--log-dest` does exactly
	 * that -- and a generic timeout sends the reader hunting in the
	 * wrong place. */
	if (prefix && dap_transport_awaiting_announce(c->t)) {
		/* The line first and the command last: if anything has to be
		 * lost to the bound it should be the argv, not the thing the
		 * reader is meant to go looking for. */
		snprintf(c->death_detail, sizeof(c->death_detail),
		    "no `%s<port>` on the standard output of `%s`", prefix,
		    c->command[0] ? c->command : "the debug adapter");
		return c->death_detail;
	}
	switch (dap_transport_error(c->t)) {
	case FRAMED_IO_ERR_EOF:
		return "the debug adapter exited";
	case FRAMED_IO_ERR_IO:
		return "the connection to the debug adapter failed";
	case FRAMED_IO_ERR_PROTOCOL:
		return dap_transport_error_truncated(c->t)
		    ? "the debug adapter stopped in the middle of a reply"
		    : "the debug adapter sent a malformed frame";
	case FRAMED_IO_ERR_TOO_LARGE:
		return dap_transport_error_outbound(c->t)
		    ? "kg could not keep up with the debug adapter"
		    : "the debug adapter sent more than kg will hold";
	case FRAMED_IO_ERR_NOMEM:
		return "kg ran out of memory for this debug adapter";
	case FRAMED_IO_OK:
		break;
	}
	return "the debug adapter exited";
}

/* The client is finished, for the first reason only: a second verdict would
 * replace the one that explains what happened with one that fits
 * everything. */
static void client_die(struct dap_client *c, const char *why)
{
	if (c->dead) {
		return;
	}
	c->dead = true;
	safe_text(why, c->death, sizeof(c->death));
	client_log(c, DAP_LOG_ERROR, c->death);
	pending_fail_all(c, c->death);
}

/* ---------------------------- capabilities ---------------------------- */

/* The wire spellings, one row per gate.  `alt` is debugpy's misspelling of
 * `supportTerminateDebuggee`, read second so the spec's spelling wins when
 * an adapter sends both [M-7]. */
static const struct {
	enum dap_capability cap;
	const char *key;
	const char *alt;
} cap_keys[] = {
	{ DAP_CAP_CONFIGURATION_DONE, "supportsConfigurationDoneRequest",
	    NULL },
	{ DAP_CAP_CONDITIONAL_BREAKPOINTS, "supportsConditionalBreakpoints",
	    NULL },
	{ DAP_CAP_HIT_CONDITIONAL_BREAKPOINTS,
	    "supportsHitConditionalBreakpoints", NULL },
	{ DAP_CAP_LOG_POINTS, "supportsLogPoints", NULL },
	{ DAP_CAP_EVALUATE_FOR_HOVERS, "supportsEvaluateForHovers", NULL },
	{ DAP_CAP_SET_VARIABLE, "supportsSetVariable", NULL },
	{ DAP_CAP_TERMINATE_REQUEST, "supportsTerminateRequest", NULL },
	{ DAP_CAP_TERMINATE_DEBUGGEE, "supportTerminateDebuggee",
	    "supportsTerminateDebuggee" },
	{ DAP_CAP_RESTART_REQUEST, "supportsRestartRequest", NULL },
	{ DAP_CAP_GOTO_TARGETS, "supportsGotoTargetsRequest", NULL },
	{ DAP_CAP_STEP_IN_TARGETS, "supportsStepInTargetsRequest", NULL },
	{ DAP_CAP_STEPPING_GRANULARITY, "supportsSteppingGranularity", NULL },
	{ DAP_CAP_DELAYED_STACK_TRACE_LOADING,
	    "supportsDelayedStackTraceLoading", NULL },
};

/* Replace the filter list, and only when one was supplied: a `capabilities`
 * event that says nothing about exception filters is not an adapter
 * withdrawing them. */
static void caps_merge_filters(
    struct dap_client *c, const struct kg_json_value *list)
{
	const struct kg_json_value *entry;
	struct dap_exception_filter *out;
	const char *id;
	size_t count;
	size_t i;

	if (kg_json_kind_of(list) != KG_JSON_ARRAY) {
		return;
	}
	count = kg_json_len(list);
	c->filter_count = 0;
	for (i = 0; i < count && c->filter_count < ARRAY_LEN(c->filters); i++) {
		entry = kg_json_at(list, i);
		id = kg_json_str(kg_json_get(entry, "filter"), NULL);
		if (!id || !*id) {
			continue;
		}
		out = &c->filters[c->filter_count++];
		memset(out, 0, sizeof(*out));
		safe_text(id, out->filter, sizeof(out->filter));
		safe_text(kg_json_str(kg_json_get(entry, "label"), NULL),
		    out->label, sizeof(out->label));
		safe_text(kg_json_str(kg_json_get(entry, "description"), NULL),
		    out->description, sizeof(out->description));
		out->default_on
		    = kg_json_bool(kg_json_get(entry, "default"), false);
		out->supports_condition = kg_json_bool(
		    kg_json_get(entry, "supportsCondition"), false);
	}
}

/* Merge, which means member-present-overwrites INCLUDING an explicit false
 * -- an adapter may turn a feature off, and lldb-dap turns two ON fifteen
 * milliseconds after an initialize response that omitted them.  Never OR.
 *
 * Absent is not false, and the tri-state that says so is the JSON kind: a
 * member that is not a bool leaves the stored value exactly as it was. */
static void caps_merge(struct dap_client *c, const struct kg_json_value *caps)
{
	const struct kg_json_value *v;
	size_t i;

	if (kg_json_kind_of(caps) != KG_JSON_OBJECT) {
		return;
	}
	for (i = 0; i < ARRAY_LEN(cap_keys); i++) {
		v = kg_json_get(caps, cap_keys[i].key);
		if (kg_json_kind_of(v) != KG_JSON_BOOL && cap_keys[i].alt) {
			v = kg_json_get(caps, cap_keys[i].alt);
		}
		if (kg_json_kind_of(v) == KG_JSON_BOOL) {
			c->caps[cap_keys[i].cap] = kg_json_bool(v, false);
		}
	}
	caps_merge_filters(c, kg_json_get(caps, "exceptionBreakpointFilters"));
}

/* ------------------------------- output ------------------------------- */

static void held_output_flush(struct dap_client *c)
{
	size_t i;

	for (i = 0; i < c->held_count && c->hooks.output; i++) {
		c->hooks.output(c->hooks.ctx, c->held[i].category,
		    c->held[i].text, c->held[i].len);
	}
	if (c->held_dropped && c->hooks.log) {
		client_log(c, DAP_LOG_DEBUG,
		    "some adapter output was dropped before there was "
		    "anywhere to put it");
	}
	c->held_count = 0;
	c->held_dropped = 0;
}

static void held_output_append(
    struct dap_client *c, const char *category, const char *text, size_t len)
{
	struct dap_held_output *slot;

	if (c->held_count >= ARRAY_LEN(c->held)) {
		c->held_dropped++;
		return;
	}
	slot = &c->held[c->held_count++];
	memset(slot, 0, sizeof(*slot));
	snprintf(slot->category, sizeof(slot->category), "%s", category);
	if (len > sizeof(slot->text)) {
		len = sizeof(slot->text);
		c->held_dropped++;
	}
	memcpy(slot->text, text, len);
	slot->len = len;
}

/* An `output` event.  Telemetry is dropped immediately and unconditionally
 * [M-12]; everything else is bytes, not lines -- debugpy splits one print
 * across two events mid-line -- and goes to the destination if there is
 * one, or waits bounded for one if there is not. */
static void handle_output(
    struct dap_client *c, const struct kg_json_value *body)
{
	const char *category = kg_json_str(kg_json_get(body, "category"), NULL);
	size_t len = 0;
	const char *text = kg_json_str(kg_json_get(body, "output"), &len);

	if (!category || !*category) {
		category = "console"; /* the protocol's own default */
	}
	if (strcmp(category, "telemetry") == 0) {
		return;
	}
	if (!text || len == 0) {
		return;
	}
	if (c->hooks.output) {
		c->hooks.output(c->hooks.ctx, category, text, len);
		return;
	}
	held_output_append(c, category, text, len);
}

/* ------------------------------- events ------------------------------- */

static void deliver_event(
    struct dap_client *c, const char *event, const struct kg_json_value *body)
{
	if (c->hooks.event) {
		c->hooks.event(c->hooks.ctx, event, body);
	}
}

/* One event, after the envelope check.  Two of them are this layer's own
 * business -- `output` has a transcript rather than a state machine for a
 * destination, and `capabilities` is the mutable capability set [M-7] --
 * and the rest, known or unknown, go to the hook: which events matter is a
 * decision for a layer that knows what a thread is. */
static void handle_event(struct dap_client *c, const struct kg_json_value *root)
{
	const char *name = kg_json_str(kg_json_get(root, "event"), NULL);
	const struct kg_json_value *body = kg_json_get(root, "body");

	if (!name || !*name) {
		client_log(c, DAP_LOG_ERROR,
		    "the debug adapter sent an event with no name");
		return;
	}
	if (strcmp(name, "output") == 0) {
		handle_output(c, body);
		return;
	}
	if (strcmp(name, "capabilities") == 0) {
		caps_merge(c, kg_json_get(body, "capabilities"));
	}
	if (strcmp(name, "initialized") == 0) {
		c->initialized_seen = true;
		if (!c->initialize_done) {
			/* Out of order, and absorbed in one bool rather than
			 * an arbitrary pre-init event store [M-13]. */
			c->initialized_deferred = true;
			return;
		}
	}
	deliver_event(c, name, body);
}

/* --------------------------- reverse requests ------------------------- */

/* The reverse requests kg refuses by name, and why.  v1 implements none of
 * them: it advertises neither `supportsRunInTerminalRequest` nor
 * `supportsStartDebuggingRequest` [M-2, M-3], so an adapter asking is one
 * that asked anyway, and the answer is an immediate `success:false`.  A
 * named arm exists only to say something truer than the default arm can;
 * the arm an implemented reverse request would add is a handler beside its
 * message, and there is no such arm yet.
 *
 * What matters is that there IS a default arm.  Accepting and ignoring
 * deadlocks a debuggee (the child config's `connect` is a promise), and
 * silence hangs the adapter outright -- dap-mode's missing default clause
 * is exactly this bug. */
static const struct {
	const char *command;
	const char *reason;
} reverse_arms[] = {
	{ "runInTerminal",
	    "kg does not run debuggees in a terminal in this version" },
	{ "startDebugging", "kg debugs one session at a time" },
};

static const char *reverse_reason(const char *command)
{
	size_t i;

	for (i = 0; i < ARRAY_LEN(reverse_arms); i++) {
		if (strcmp(command, reverse_arms[i].command) == 0) {
			return reverse_arms[i].reason;
		}
	}
	return NULL;
}

static void send_refusal(struct dap_client *c, int32_t request_seq,
    const char *command, const char *reason)
{
	size_t len = 0;
	int32_t seq;
	char *msg;

	if (!seq_next(c, &seq)) {
		return;
	}
	msg = build_refusal(seq, request_seq, command, reason, &len);
	if (!msg) {
		client_die(c, "kg ran out of memory answering the adapter");
		return;
	}
	(void)dap_transport_send(c->t, msg, len);
	free(msg);
}

/* A request from the adapter.  It needs a command to be answered about and
 * an inbound `seq` to be answered TO: with no correlatable number there is
 * no answer that could reach the right question, so it is logged and left,
 * never guessed at. */
static void handle_reverse_request(
    struct dap_client *c, const struct kg_json_value *root)
{
	char command[DAP_CLIENT_COMMAND_MAX];
	char text[DAP_CLIENT_COMMAND_MAX + 64];
	const char *reason;
	int32_t request_seq;

	safe_text(kg_json_str(kg_json_get(root, "command"), NULL), command,
	    sizeof(command));
	if (!command[0]) {
		client_log(c, DAP_LOG_ERROR,
		    "the debug adapter sent a request with no command");
		return;
	}
	if (!id_int32(kg_json_get(root, "seq"), &request_seq)) {
		snprintf(text, sizeof(text),
		    "the adapter's %s request has no usable seq", command);
		client_log(c, DAP_LOG_ERROR, text);
		return;
	}
	reason = reverse_reason(command);
	if (!reason) {
		snprintf(
		    text, sizeof(text), "kg implements no %s request", command);
		reason = text;
	}
	client_log(c, DAP_LOG_DEBUG, reason);
	send_refusal(c, request_seq, command, reason);
}

/* ------------------------------ responses ----------------------------- */

/* Why a request failed [M-6]: `message` first (debugpy), then
 * `body.error.format` (lldb-dap, which puts it nowhere else), then a
 * sentence kg synthesises so that `message` is never empty.  `showUser`,
 * `url` and `urlLabel` are kept as the adapter sent them.
 *
 * The format string's `{variable}` placeholders are NOT interpolated: kg
 * would be composing adapter text into adapter text, and both measured
 * adapters send the format already resolved. */
static void extract_error(const struct kg_json_value *root, const char *command,
    struct dap_error *out)
{
	const struct kg_json_value *err
	    = kg_json_get(kg_json_get(root, "body"), "error");
	const char *text = kg_json_str(kg_json_get(root, "message"), NULL);
	char synthesised[DAP_CLIENT_COMMAND_MAX + 32];

	memset(out, 0, sizeof(*out));
	if (!text || !*text) {
		text = kg_json_str(kg_json_get(err, "format"), NULL);
	}
	if (!text || !*text) {
		snprintf(synthesised, sizeof(synthesised),
		    "the adapter refused %s", command);
		text = synthesised;
	}
	safe_text(text, out->message, sizeof(out->message));
	safe_text(kg_json_str(kg_json_get(err, "url"), NULL), out->url,
	    sizeof(out->url));
	safe_text(kg_json_str(kg_json_get(err, "urlLabel"), NULL),
	    out->url_label, sizeof(out->url_label));
	out->show_user = kg_json_bool(kg_json_get(err, "showUser"), false);
}

/* Run a matched response's callback.  The `initialize` reply is the one
 * this layer reads on its own way past: capabilities are merged BEFORE the
 * callback, so a session sending `launch` from there already sees
 * dap_capable()'s live answers, and an `initialized` event that arrived
 * early is delivered after it, in the order the adapter should have sent
 * them. */
static void response_deliver(struct dap_client *c,
    const struct dap_pending *slot, const struct kg_json_value *root)
{
	bool is_initialize = strcmp(slot->command, "initialize") == 0;
	struct dap_error err;
	struct dap_response r;

	memset(&r, 0, sizeof(r));
	r.command = slot->command;
	r.answered = true;
	r.success = kg_json_bool(kg_json_get(root, "success"), false);
	r.body = kg_json_get(root, "body");
	if (!r.success) {
		extract_error(root, slot->command, &err);
		r.error = &err;
	}
	if (is_initialize) {
		c->initialize_done = true;
		if (r.success) {
			caps_merge(c, r.body);
		}
	}
	if (slot->cb) {
		slot->cb(c, &r, slot->ctx);
	}
	if (is_initialize && c->initialized_deferred) {
		c->initialized_deferred = false;
		deliver_event(c, "initialized", NULL);
	}
}

/* A response, matched by `request_seq` and by nothing else [M-5].  Three
 * things are refused rather than guessed at: an envelope missing one of the
 * three members a response must carry, a number nobody is waiting for
 * (a duplicate, or the answer to a request already abandoned), and a body
 * whose `command` disagrees with the slot -- which is an adapter answering
 * the wrong question, so the slot stays pending for the right one. */
static void handle_response(
    struct dap_client *c, const struct kg_json_value *root)
{
	const char *command = kg_json_str(kg_json_get(root, "command"), NULL);
	const struct kg_json_value *ok = kg_json_get(root, "success");
	char text[DAP_CLIENT_COMMAND_MAX * 2 + 64];
	struct dap_pending slot;
	int32_t request_seq;
	size_t index;

	if (!id_int32(kg_json_get(root, "request_seq"), &request_seq)
	    || kg_json_kind_of(ok) != KG_JSON_BOOL || !command) {
		client_log(c, DAP_LOG_ERROR,
		    "the debug adapter sent a response kg cannot correlate");
		return;
	}
	index = pending_find(c, request_seq);
	if (index == DAP_CLIENT_MAX_PENDING) {
		client_log(c, DAP_LOG_DEBUG,
		    "the debug adapter answered a request nobody is waiting "
		    "for");
		return;
	}
	if (strcmp(c->pending[index].command, command) != 0) {
		snprintf(text, sizeof(text),
		    "the adapter answered %s with a %s response",
		    c->pending[index].command, command);
		client_log(c, DAP_LOG_ERROR, text);
		return;
	}
	slot = c->pending[index];
	memset(&c->pending[index], 0, sizeof(c->pending[index]));
	response_deliver(c, &slot, root);
}

/* ------------------------------ dispatching --------------------------- */

enum dap_message_type {
	DAP_MESSAGE_UNKNOWN = 0,
	DAP_MESSAGE_REQUEST,
	DAP_MESSAGE_RESPONSE,
	DAP_MESSAGE_EVENT,
};

static enum dap_message_type message_type(const struct kg_json_value *root)
{
	const char *type = kg_json_str(kg_json_get(root, "type"), NULL);

	if (!type) {
		return DAP_MESSAGE_UNKNOWN;
	}
	if (strcmp(type, "response") == 0) {
		return DAP_MESSAGE_RESPONSE;
	}
	if (strcmp(type, "event") == 0) {
		return DAP_MESSAGE_EVENT;
	}
	if (strcmp(type, "request") == 0) {
		return DAP_MESSAGE_REQUEST;
	}
	return DAP_MESSAGE_UNKNOWN;
}

/* One complete message: parsed into a document that owns its bytes (so the
 * transport's buffer may be reused the moment this returns), sorted by
 * `type`, and released.  Bytes that are not JSON at all end the client --
 * the framing was intact, so what is inside it is the adapter's own doing
 * -- while a JSON message with a type kg does not know is logged and
 * ignored, which is what the protocol asks of both sides [M-13]. */
static void dispatch_message(struct dap_client *c, const char *body, size_t len)
{
	struct kg_json *doc = kg_json_parse(body, len, NULL);
	const struct kg_json_value *root;

	if (!doc) {
		client_die(
		    c, "the debug adapter sent a message that is not JSON");
		return;
	}
	root = kg_json_root(doc);
	c->in_dispatch = true;
	switch (message_type(root)) {
	case DAP_MESSAGE_RESPONSE:
		handle_response(c, root);
		break;
	case DAP_MESSAGE_EVENT:
		handle_event(c, root);
		break;
	case DAP_MESSAGE_REQUEST:
		handle_reverse_request(c, root);
		break;
	case DAP_MESSAGE_UNKNOWN:
		client_log(c, DAP_LOG_ERROR,
		    "the debug adapter sent a message with no usable type");
		break;
	}
	c->in_dispatch = false;
	kg_json_free(doc);
}

/* An adapter whose standard error carries the DEBUGGEE's output rather than
 * its own diagnostics.  The bytes go to the output hook as bytes, on the
 * `stderr` category, and are never cut into lines: the program printed what
 * it printed, and a newline invented at a chunk boundary would be kg
 * rewriting it.  Which adapters are like this is the SPEC's answer
 * (dap_adapter_spec::route_stderr_to_output), so nothing here knows a
 * language. */
static void drain_stderr_bytes(struct dap_client *c)
{
	const char *data = NULL;
	size_t len = 0;

	while (dap_transport_next_stderr_bytes(c->t, &data, &len) == 1) {
		if (c->hooks.output) {
			c->hooks.output(c->hooks.ctx, "stderr", data, len);
		}
	}
}

/* Whatever the adapter has written to its standard error, a line at a time.
 * Read even with nobody listening: a pipe kg never reads is one a chatty
 * adapter eventually blocks writing to, and an adapter blocked on its
 * stderr is one that has stopped answering. */
static void drain_log(struct dap_client *c)
{
	char text[DAP_CLIENT_TEXT_MAX];
	const char *line = NULL;
	size_t len = 0;

	while (dap_transport_next_log_line(c->t, &line, &len) == 1) {
		if (!c->hooks.log) {
			continue;
		}
		safe_textn(line, len, text, sizeof(text));
		client_log(c, DAP_LOG_DEBUG, text);
	}
}

/* Whether this poll has done as much as it may.  The first message is
 * always dispatched however large, so one legal oversized body is never
 * stuck behind its own size; after that both budgets bite. */
/* TEST ONLY.  A poll that dispatches a whole burst is correct but
 * unobservable from outside: a suite asserting on a phase a later message
 * in the same burst leaves again -- STOPPED, then the continued right
 * behind it -- needs one message per poll to see every transition.  Zero
 * restores the built-in bound. */
static unsigned g_max_messages_for_test;

void dap_client_set_max_messages_per_poll_for_test(unsigned n)
{
	g_max_messages_for_test = n;
}

static bool budget_spent(unsigned count, size_t bytes)
{
	if (g_max_messages_for_test > 0) {
		return count >= g_max_messages_for_test;
	}
	return count >= DAP_CLIENT_MAX_MESSAGES_PER_POLL
	    || (count > 0 && bytes >= DAP_CLIENT_MAX_BYTES_PER_POLL);
}

/* ------------------------------ public API ---------------------------- */

struct dap_client *dap_client_new(struct dap_transport *t)
{
	struct dap_client *c;

	if (!t) {
		return NULL;
	}
	c = calloc(1, sizeof(*c));
	if (!c) {
		dap_transport_close(t);
		return NULL;
	}
	c->t = t;
	c->next_seq = 1;
	c->initialize_ms = DAP_TRANSPORT_INITIALIZE_TIMEOUT_MS;
	c->request_ms = DAP_TRANSPORT_REQUEST_TIMEOUT_MS;
	return c;
}

void dap_client_close(struct dap_client *c)
{
	if (!c) {
		return;
	}
	/* The last chance anybody has to read what this adapter said: the
	 * descriptors go a few lines below. */
	drain_stderr_bytes(c);
	drain_log(c);
	pending_fail_all(c, "the debug session ended");
	c->dead = true;
	dap_transport_close(c->t);
	free(c);
}

void dap_client_set_adapter_stderr_to_output(struct dap_client *c, bool on)
{
	c->stderr_to_output = on;
	dap_transport_set_stderr_bytes(c->t, on);
}

void dap_client_set_adapter_command(struct dap_client *c, const char *command)
{
	snprintf(c->command, sizeof(c->command), "%s", command ? command : "");
}

void dap_client_set_hooks(
    struct dap_client *c, const struct dap_client_hooks *hooks)
{
	if (hooks) {
		c->hooks = *hooks;
	} else {
		memset(&c->hooks, 0, sizeof(c->hooks));
	}
	held_output_flush(c);
}

void dap_client_set_timeouts(
    struct dap_client *c, unsigned initialize_ms, unsigned request_ms)
{
	c->initialize_ms = initialize_ms;
	c->request_ms = request_ms;
}

/* How long this command may go unanswered.  Launch and attach are the one
 * exemption [M-1]; `initialize` gets its own window because it is the step
 * that proves an adapter is an adapter, and everything else gets the
 * ordinary request deadline (src/dap_transport.h defines all three). */
static long long request_deadline(
    const struct dap_client *c, const char *command)
{
	unsigned limit = c->request_ms;

	if (strcmp(command, "launch") == 0 || strcmp(command, "attach") == 0) {
		return 0;
	}
	if (strcmp(command, "initialize") == 0) {
		limit = c->initialize_ms;
	}
	return limit > 0 ? now_ms() + (long long)limit : 0;
}

/* A request that never went out.  The callback is failed here and now --
 * the one place in this module where it runs before the call that
 * registered it returns -- because the alternative is a caller's context
 * released on some paths and not others. */
static long long request_refuse(struct dap_client *c, const char *command,
    dap_response_fn cb, void *ctx, const char *why)
{
	struct dap_pending slot;

	memset(&slot, 0, sizeof(slot));
	snprintf(
	    slot.command, sizeof(slot.command), "%s", command ? command : "");
	slot.cb = cb;
	slot.ctx = ctx;
	client_log(c, DAP_LOG_ERROR, why);
	fail_callback(c, &slot, why);
	return -1;
}

long long dap_client_request(struct dap_client *c, const char *command,
    const char *arguments, size_t arguments_len, dap_response_fn cb, void *ctx)
{
	struct dap_pending *slot;
	size_t len = 0;
	int32_t seq;
	char *msg;

	if (!command || strlen(command) >= DAP_CLIENT_COMMAND_MAX) {
		return request_refuse(
		    c, command, cb, ctx, "kg cannot name that request");
	}
	if (c->dead) {
		return request_refuse(c, command, cb, ctx,
		    c->death[0] ? c->death : "the debug session has ended");
	}
	slot = pending_alloc(c);
	if (!slot) {
		return request_refuse(c, command, cb, ctx,
		    "too many debugger requests are already in flight");
	}
	if (!seq_next(c, &seq)) {
		return request_refuse(c, command, cb, ctx, c->death);
	}
	msg = build_request(seq, command, arguments, arguments_len, &len);
	if (!msg || dap_transport_send(c->t, msg, len) != 0) {
		free(msg);
		return request_refuse(c, command, cb, ctx,
		    "the request could not be sent to the debug adapter");
	}
	free(msg);
	slot->seq = seq;
	slot->cb = cb;
	slot->ctx = ctx;
	slot->deadline_ms = request_deadline(c, command);
	snprintf(slot->command, sizeof(slot->command), "%s", command);
	slot->used = true;
	return seq;
}

long long dap_client_initialize(
    struct dap_client *c, const char *adapter_id, dap_response_fn cb, void *ctx)
{
	size_t len = 0;
	char *args = build_initialize_args(adapter_id, &len);
	long long seq;

	if (!args) {
		return request_refuse(c, "initialize", cb, ctx,
		    "kg ran out of memory starting the debug adapter");
	}
	seq = dap_client_request(c, "initialize", args, len, cb, ctx);
	free(args);
	return seq;
}

int dap_client_poll(struct dap_client *c)
{
	const char *body = NULL;
	size_t bytes = 0;
	size_t len = 0;
	unsigned count = 0;
	int changed = 0;
	int rc;

	/* Handlers send and flush; they never re-enter the read loop.  The
	 * assertion is the contract and the flag is what makes the work
	 * still happen, on the caller's next poll. */
	assert(!c->in_dispatch);
	if (c->in_dispatch) {
		c->poll_again = true;
		return 0;
	}
	drain_stderr_bytes(c);
	drain_log(c);
	if (c->dead) {
		return 0;
	}
	c->poll_again = false;
	if (dap_transport_pending_bytes(c->t) > 0) {
		(void)dap_transport_flush(c->t);
	}
	for (;;) {
		if (budget_spent(count, bytes)) {
			c->poll_again = true;
			break;
		}
		rc = dap_transport_next_message(c->t, &body, &len);
		if (rc < 0) {
			client_die(c, transport_death_text(c));
			return 1;
		}
		if (rc == 0) {
			break;
		}
		count++;
		bytes += len;
		dispatch_message(c, body, len);
		changed = 1;
		if (c->dead) {
			return 1;
		}
	}
	/* Last, and only while alive: a request whose deadline passed as its
	 * adapter died has already been failed by the death path, and
	 * reporting it twice is the one thing the contract forbids. */
	return changed | pending_expire(c);
}

bool dap_client_wants_poll(const struct dap_client *c) { return c->poll_again; }

int dap_client_wait_fds(const struct dap_client *c, int *fds, int max)
{
	return c->dead ? 0 : dap_transport_wait_fds(c->t, fds, max);
}

bool dap_client_alive(const struct dap_client *c) { return !c->dead; }

const char *dap_client_death_text(const struct dap_client *c)
{
	return c->death;
}

bool dap_capable(const struct dap_client *c, enum dap_capability cap)
{
	return (size_t)cap < DAP_CAP__COUNT && c->caps[cap];
}

size_t dap_client_exception_filter_count(const struct dap_client *c)
{
	return c->filter_count;
}

const struct dap_exception_filter *dap_client_exception_filter(
    const struct dap_client *c, size_t index)
{
	return index < c->filter_count ? &c->filters[index] : NULL;
}

bool dap_client_initialized_seen(const struct dap_client *c)
{
	return c->initialized_seen;
}

size_t dap_client_pending_count(const struct dap_client *c)
{
	size_t count = 0;
	size_t i;

	for (i = 0; i < DAP_CLIENT_MAX_PENDING; i++) {
		count += c->pending[i].used ? 1u : 0u;
	}
	return count;
}

struct dap_transport *dap_client_transport(struct dap_client *c)
{
	return c->t;
}
