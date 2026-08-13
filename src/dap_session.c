/* ========================= one debugging session ========================
 *
 * See src/dap_session.h for the contract: the phase, the milestones, the
 * configuration join and the teardown matrix, and why each of them is
 * shaped the way it is.  Stage 4 of doc/plans/dap/01-protocol.md.
 *
 * Everything here happens in a callback or an event handler, and none of
 * them blocks: dap_session_poll() is what moves the conversation, from the
 * editor's two poll sites through src/dap_core.c.
 */

#include "dap_session.h"

#include "dap.h"
#include "dap_transport.h"
#include "json.h"
#include "process.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

struct dap_client; /* src/dap_client.h */
struct kg_json_value; /* src/json.h */

/* The facade's wait bound is this module's, and the sum in src/async.c is
 * built on it.  A session that could hold more descriptors than the editor
 * will wait on would be a session heard only on the idle tick, so this is a
 * compile-time question rather than a truncated wait. */
static_assert(KG_DAP_WAIT_FDS_MAX
	== DAP_SESSION_MAX_SESSIONS * DAP_TRANSPORT_WAIT_FDS_MAX,
    "KG_DAP_WAIT_FDS_MAX must be every session's transport descriptors");

/* One outstanding `setBreakpoints`, as its callback's context: which
 * session, and which entry of the SNAPSHOT it belongs to.  The response is
 * matched against the snapshot rather than against whatever the breakpoint
 * table holds by then, which is dape's own hazard (dape.el:1760) written
 * down. */
struct dap_source_slot {
	struct dap_session *session;
	size_t index;
};

struct dap_session {
	struct dap_client *client;
	struct dap_session_hooks hooks;
	enum dap_request_kind request;
	char adapter_id[DAP_CONFIG_NAME_MAX];
	char config_name[DAP_CONFIG_NAME_MAX];
	char *arguments;
	size_t arguments_len;
	struct dap_session_state st;
	/* The ladder has been entered (so a second cause does not restart
	 * it), the transport's own shutdown is running, and the session is
	 * inside become_dead().  The last one matters because closing a
	 * client RUNS its pending callbacks, and one of those callbacks --
	 * the launch that will now never answer -- would otherwise ask for
	 * a teardown of the session that is already being torn down. */
	bool ending;
	bool closing;
	bool dying;
	struct dap_session_source *sources;
	struct dap_source_slot *slots;
	/* v1 debugs one session at a time [decision 3], but the shape the
	 * child-session follow-on needs is here from the first commit so
	 * that change stays inside the oracle below. */
	struct dap_session *parent;
	struct dap_session *children[DAP_SESSION_MAX_SESSIONS];
	size_t child_count;
	/* Where the ADAPTER runs, kept because the diagnostics a failed
	 * launch produces are relative to it and are read long after the
	 * spec that said so has gone out of scope. */
	char adapter_cwd[PATH_MAX];
	/* Everything the adapter wrote to the `stderr` category before it
	 * answered launch/attach.  For an adapter that BUILDS the program it
	 * is about to debug, that is the compiler's diagnostics, and the
	 * launch response itself carries only "check the debug console"
	 * (measured against delve, doc/plans/dap/04-go.md) -- so the useful
	 * text arrives before the failure it explains and has to be held
	 * until kg knows whether it mattered.
	 *
	 * Bounded, because "hold it until we know" without a bound is a
	 * chatty adapter's memory leak; the bound and the truncation notice
	 * are compilation's own policy, since this text ends up in the same
	 * buffer. */
	char *launch_output;
	size_t launch_output_len;
	size_t launch_output_cap;
	bool launch_output_truncated;
};

static struct dap_session *g_sessions[DAP_SESSION_MAX_SESSIONS];

/* Defined with the output hook that fills the buffer, used by the launch
 * response that empties it. */
static void launch_output_release(struct dap_session *s);

/* ------------------------------ the matrix ---------------------------- */

static enum dap_end_action terminate_action(
    bool can_terminate, bool can_terminate_debuggee)
{
	if (can_terminate) {
		return DAP_END_ACTION_TERMINATE;
	}
	if (can_terminate_debuggee) {
		return DAP_END_ACTION_DISCONNECT_TERMINATE;
	}
	/* Neither: there is no way to ask for the debuggee's death, so the
	 * session detaches rather than pretending it did something. */
	return DAP_END_ACTION_DISCONNECT_KEEP;
}

enum dap_end_action dap_session_end_action(enum dap_end_request request,
    bool attached, bool can_terminate, bool can_terminate_debuggee)
{
	switch (request) {
	case DAP_END_NATURAL:
		return DAP_END_ACTION_CLOSE;
	case DAP_END_DISCONNECT:
		return DAP_END_ACTION_DISCONNECT_KEEP;
	case DAP_END_TERMINATE:
		return terminate_action(can_terminate, can_terminate_debuggee);
	case DAP_END_CLEANUP:
		/* An attached debuggee is somebody else's program and must
		 * outlive the editor; a launched one is kg's to end. */
		return attached
		    ? DAP_END_ACTION_DISCONNECT_KEEP
		    : terminate_action(can_terminate, can_terminate_debuggee);
	case DAP_END_LAUNCH_FAILURE:
		return attached ? DAP_END_ACTION_CLOSE
				: DAP_END_ACTION_DISCONNECT_KEEP;
	}
	return DAP_END_ACTION_CLOSE;
}

/* ------------------------------ reporting ----------------------------- */

static void session_changed(struct dap_session *s)
{
	if (s->hooks.changed) {
		s->hooks.changed(s->hooks.ctx, s);
	}
}

static void session_report(
    struct dap_session *s, bool error, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	// NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
	vsnprintf(s->st.status, sizeof(s->st.status), fmt, ap);
	va_end(ap);
	if (s->hooks.report) {
		s->hooks.report(s->hooks.ctx, error, s->st.status);
	}
}

/* ---------------------------- state transitions ----------------------- */

/* Idempotent, and one-way: reachable from the launch response OR the
 * configurationDone response [M-1], and never from a session that has
 * already stopped -- a `stopped` that arrived while configurationDone was
 * still pending [M-10] must not be undone by the response to it. */
static void become_active(struct dap_session *s)
{
	if (s->st.phase != DAP_PHASE_STARTING) {
		return;
	}
	s->st.phase = DAP_PHASE_ACTIVE;
	session_changed(s);
}

static void become_dead(struct dap_session *s, const char *why)
{
	struct dap_client *client = s->client;
	char reason[DAP_SESSION_TEXT_MAX];

	if (s->st.phase == DAP_PHASE_DEAD || s->dying) {
		return;
	}
	/* Copied before the close, because one caller's `why` is
	 * dap_client_death_text() -- storage inside the very client this is
	 * about to free. */
	snprintf(reason, sizeof(reason), "%s", why ? why : "");
	/* Detached before the close, so that a callback the close runs finds
	 * a session with nothing left to send on. */
	s->dying = true;
	s->ending = true;
	s->client = NULL;
	s->closing = false;
	/* Closing the client is what flushes every pending callback with a
	 * synthetic failure, exactly once, before anything is freed. */
	dap_client_close(client);
	s->st.phase = DAP_PHASE_DEAD;
	s->st.thread_id = 0;
	session_report(s, false, "%s", reason);
	session_changed(s);
}

/* ---------------------------- sending requests ------------------------ */

/* A request whose body could not even be built.  Its callback still runs
 * exactly once, here, because the caller's context is released on that path
 * as on every other. */
static void fail_locally(
    struct dap_session *s, const char *command, dap_response_fn cb, void *ctx)
{
	struct dap_response r = { command, false, false, NULL, NULL };

	cb(s->client, &r, ctx);
}

/* Whether the adapter said it can do something.  A session with no client
 * left can do nothing, which is the honest answer for every capability. */
static bool session_capable(
    const struct dap_session *s, enum dap_capability cap)
{
	return s->client && dap_capable(s->client, cap);
}

static void send_built(struct dap_session *s, const char *command,
    struct kg_jsonw *w, dap_response_fn cb, void *ctx)
{
	char *body = NULL;
	size_t len = 0;

	if (!s->client) {
		kg_jsonw_free(w);
		fail_locally(s, command, cb, ctx);
		return;
	}
	if (kg_jsonw_finish(w, &body, &len) != 0) {
		fail_locally(s, command, cb, ctx);
		return;
	}
	/* A request that cannot be sent fails its callback locally and
	 * synchronously (src/dap_client.h), so there is nothing to do with
	 * the return value here. */
	(void)dap_client_request(s->client, command, body, len, cb, ctx);
	free(body);
}

/* -------------------------- the configuration join -------------------- */

static void configuration_finished(struct dap_session *s)
{
	s->st.configuration_done = true;
	become_active(s);
	session_changed(s);
}

static void on_configuration_done(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct dap_session *s = ctx;

	(void)c;
	if (!r->success) {
		session_report(s, true, "configurationDone failed: %s",
		    r->error ? r->error->message : "no answer");
	}
	configuration_finished(s);
}

/* `configurationDone` goes out only if the adapter advertised it; without
 * the capability the milestone completes after exception configuration and
 * nothing is sent, which is what the capability MEANS. */
static void send_configuration_done(struct dap_session *s)
{
	struct kg_jsonw w;

	if (!session_capable(s, DAP_CAP_CONFIGURATION_DONE)) {
		configuration_finished(s);
		return;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_end_object(&w);
	s->st.configuration_done_sent = true;
	send_built(s, "configurationDone", &w, on_configuration_done, s);
}

static void on_exception_breakpoints(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct dap_session *s = ctx;

	(void)c;
	if (!r->success) {
		session_report(s, true, "setExceptionBreakpoints failed: %s",
		    r->error ? r->error->message : "no answer");
	}
	send_configuration_done(s);
}

/* Built from the LIVE filter set, honouring each filter's own `default`
 * [M-7] -- which is where Python's uncaught-exception stops come from for
 * free, since debugpy's `uncaught` filter defaults true.  An adapter that
 * advertises no filters is asked nothing: an empty request would be a
 * capability kg invented. */
static void send_exception_breakpoints(struct dap_session *s)
{
	size_t count
	    = s->client ? dap_client_exception_filter_count(s->client) : 0;
	const struct dap_exception_filter *filter;
	struct kg_jsonw w;
	size_t i;

	if (count == 0) {
		send_configuration_done(s);
		return;
	}
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "filters");
	kg_jsonw_begin_array(&w);
	for (i = 0; i < count; i++) {
		filter = dap_client_exception_filter(s->client, i);
		if (filter && filter->default_on) {
			kg_jsonw_string(&w, filter->filter);
		}
	}
	kg_jsonw_end_array(&w);
	kg_jsonw_end_object(&w);
	send_built(
	    s, "setExceptionBreakpoints", &w, on_exception_breakpoints, s);
}

/* The join's end: every source has answered, however it answered.  One
 * failed source is reported and counted and the sequence continues -- a
 * breakpoint kg could not set is not a reason to leave a session half
 * configured forever. */
static void on_set_breakpoints(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct dap_source_slot *slot = ctx;
	struct dap_session *s = slot->session;

	(void)c;
	s->st.sources_answered++;
	if (!r->success) {
		s->st.sources_failed++;
		session_report(s, true, "breakpoints in %s were refused: %s",
		    s->sources[slot->index].path,
		    r->error ? r->error->message : "no answer");
	}
	/* The provider's own answer: this response is about the snapshot IT
	 * handed over, and its ids and verdicts are in the body. */
	if (s->hooks.breakpoints_answered) {
		s->hooks.breakpoints_answered(s->hooks.ctx,
		    s->sources[slot->index].path, r->success, r->body);
	}
	if (s->st.sources_answered >= s->st.sources_sent) {
		send_exception_breakpoints(s);
	}
}

static void send_set_breakpoints(struct dap_session *s, size_t index)
{
	const struct dap_session_source *src = &s->sources[index];
	struct kg_jsonw w;
	size_t i;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "source");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "path");
	kg_jsonw_string(&w, src->path);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "breakpoints");
	kg_jsonw_begin_array(&w);
	for (i = 0; i < src->line_count; i++) {
		kg_jsonw_begin_object(&w);
		kg_jsonw_key(&w, "line");
		kg_jsonw_int(&w, src->lines[i]);
		kg_jsonw_end_object(&w);
	}
	kg_jsonw_end_array(&w);
	kg_jsonw_key(&w, "sourceModified");
	kg_jsonw_bool(&w, false);
	kg_jsonw_end_object(&w);
	send_built(
	    s, "setBreakpoints", &w, on_set_breakpoints, &s->slots[index]);
}

/* The snapshot, taken once: the sources the join is about are these, and a
 * table that changes while the responses are in flight changes the next
 * round rather than this one. */
static size_t snapshot_sources(struct dap_session *s)
{
	size_t count;

	if (!s->hooks.sources) {
		return 0;
	}
	s->sources = calloc(DAP_SESSION_MAX_SOURCES, sizeof(*s->sources));
	s->slots = calloc(DAP_SESSION_MAX_SOURCES, sizeof(*s->slots));
	if (!s->sources || !s->slots) {
		return 0;
	}
	count = s->hooks.sources(
	    s->hooks.ctx, s->sources, DAP_SESSION_MAX_SOURCES);
	return count > DAP_SESSION_MAX_SOURCES ? DAP_SESSION_MAX_SOURCES
					       : count;
}

/* Configuration starts EXACTLY ONCE, from both halves being true, and
 * either half may become true first: adapters send `initialized` before the
 * initialize response [M-13] as readily as after it. */
static void maybe_start_configuration(struct dap_session *s)
{
	size_t count;
	size_t i;

	if (s->st.configuration_started || !s->st.initialized_seen
	    || !s->st.initialize_done || s->ending) {
		return;
	}
	s->st.configuration_started = true;
	count = snapshot_sources(s);
	s->st.sources_sent = count;
	if (count == 0) {
		send_exception_breakpoints(s);
		return;
	}
	for (i = 0; i < count; i++) {
		s->slots[i].session = s;
		s->slots[i].index = i;
	}
	for (i = 0; i < count; i++) {
		send_set_breakpoints(s, i);
	}
}

/* ------------------------------- handshake ---------------------------- */

static void on_launch(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct dap_session *s = ctx;

	(void)c;
	s->st.launch_done = true;
	if (r->success) {
		/* Nothing to explain, so nothing to keep. */
		launch_output_release(s);
		become_active(s);
		session_changed(s);
		return;
	}
	if (s->launch_output_len > 0 && s->hooks.build_failed) {
		s->hooks.build_failed(s->hooks.ctx, s->config_name,
		    s->adapter_cwd, s->launch_output, s->launch_output_len,
		    s->launch_output_truncated);
	}
	launch_output_release(s);
	/* Retained and reported, and the configuration traffic the adapter
	 * is still emitting keeps running (measured): the sequence finishes,
	 * configurationDone fails, and the session tears down rather than
	 * waiting for something that will never come. */
	s->st.launch_failed = true;
	session_report(s, true, "%s failed: %s",
	    s->request == DAP_REQUEST_ATTACH ? "attach" : "launch",
	    r->error ? r->error->message : "no answer");
	dap_session_end(s, DAP_END_LAUNCH_FAILURE);
}

/* Sent from the initialize response handler, with NO deadline [M-1]: the
 * response has no fixed position and a debuggee may take as long as it
 * likes to start.  What replaces the deadline is dap_session_cancel().
 *
 * A teardown that began while `initialize` was still outstanding wins here,
 * and only here: the configuration continuations deliberately run to the
 * end of an ending session, because they FINISH what the adapter is already
 * doing, while this one would START the debuggee the user just asked to
 * stop. */
static void send_launch(struct dap_session *s)
{
	const char *command
	    = s->request == DAP_REQUEST_ATTACH ? "attach" : "launch";

	if (s->ending) {
		return;
	}
	s->st.launch_sent = true;
	if (!s->client) {
		fail_locally(s, command, on_launch, s);
		return;
	}
	(void)dap_client_request(
	    s->client, command, s->arguments, s->arguments_len, on_launch, s);
}

static void on_initialize(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct dap_session *s = ctx;

	s->st.initialize_done = true;
	if (!r->success) {
		session_report(s, true,
		    "the debug adapter refused initialize: %s",
		    r->error ? r->error->message : "no answer");
		dap_session_end(s, DAP_END_LAUNCH_FAILURE);
		return;
	}
	send_launch(s);
	/* An `initialized` that arrived before this response is remembered
	 * by the client as one bool; reading it here rather than waiting for
	 * the deferred delivery makes the order deterministic. */
	if (dap_client_initialized_seen(c)) {
		s->st.initialized_seen = true;
	}
	maybe_start_configuration(s);
	session_changed(s);
}

/* -------------------------------- events ------------------------------ */

static void on_stopped(struct dap_session *s, const struct kg_json_value *body)
{
	if (s->st.phase == DAP_PHASE_DEAD || s->ending) {
		return;
	}
	/* Safe while the session is nominally still configuring: lldb-dap
	 * with stopOnEntry stops before the configurationDone response
	 * [M-10].  The stop model proper -- threads, frames, the suspension
	 * epoch -- is stage 6; what the session owns is the phase and which
	 * thread the news was about. */
	s->st.thread_id = (int)kg_json_int(kg_json_get(body, "threadId"), 0);
	s->st.phase = DAP_PHASE_STOPPED;
	session_changed(s);
}

static void on_continued(struct dap_session *s) { dap_session_note_resumed(s); }

/* The three transitions stage 6 owns (src/dap_session.h).  Each is
 * idempotent and each refuses from every phase but the one it names, so a
 * caller never has to know which phase the session is in to be safe. */
bool dap_session_begin_resume(struct dap_session *s)
{
	if (!s || s->st.phase != DAP_PHASE_STOPPED) {
		return false;
	}
	s->st.phase = DAP_PHASE_RESUMING;
	session_changed(s);
	return true;
}

void dap_session_note_stopped(struct dap_session *s)
{
	if (!s || s->st.phase != DAP_PHASE_RESUMING) {
		return;
	}
	s->st.phase = DAP_PHASE_STOPPED;
	session_changed(s);
}

void dap_session_note_resumed(struct dap_session *s)
{
	if (!s
	    || (s->st.phase != DAP_PHASE_STOPPED
		&& s->st.phase != DAP_PHASE_RESUMING)) {
		return;
	}
	s->st.phase = DAP_PHASE_ACTIVE;
	s->st.thread_id = 0;
	session_changed(s);
}

/* `exited` is the debuggee's exit code and NOT the end of the session
 * [M-8]: the adapter is still there, and `terminated` (or the transport)
 * is what ends this. */
static void on_exited(struct dap_session *s, const struct kg_json_value *body)
{
	s->st.exited = true;
	s->st.exit_code = (int)kg_json_int(kg_json_get(body, "exitCode"), 0);
	s->st.thread_id = 0;
	if (s->st.phase == DAP_PHASE_STOPPED) {
		s->st.phase = DAP_PHASE_ACTIVE;
	}
	session_report(
	    s, false, "the program exited with code %d", s->st.exit_code);
	session_changed(s);
}

static void on_terminated(
    struct dap_session *s, const struct kg_json_value *body)
{
	const struct kg_json_value *restart = kg_json_get(body, "restart");

	s->st.terminated_seen = true;
	/* Retained for the restart policy: any value but an explicit false
	 * is the adapter asking to be restarted with it. */
	if (kg_json_kind_of(restart) != KG_JSON_NONE
	    && !(kg_json_kind_of(restart) == KG_JSON_BOOL
		&& !kg_json_bool(restart, false))) {
		s->st.restart_requested = true;
	}
	/* Idempotent, which is what tolerates the duplicate `terminated`
	 * delve sends. */
	dap_session_end(s, DAP_END_NATURAL);
}

static void on_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	struct dap_session *s = ctx;

	if (strcmp(event, "initialized") == 0) {
		s->st.initialized_seen = true;
		maybe_start_configuration(s);
	} else if (strcmp(event, "stopped") == 0) {
		on_stopped(s, body);
	} else if (strcmp(event, "continued") == 0) {
		on_continued(s);
	} else if (strcmp(event, "exited") == 0) {
		on_exited(s, body);
	} else if (strcmp(event, "terminated") == 0) {
		on_terminated(s, body);
	}
	/* Everything is forwarded, handled or not: which events matter to a
	 * pane is not this layer's decision. */
	if (s->hooks.event) {
		s->hooks.event(s->hooks.ctx, event, body);
	}
}

/* Retain launch-phase diagnostics, growing to the bound and then recording
 * that the rest was dropped.  The bytes are kept exactly as they arrived:
 * one `output` event is not one line [M-12], and re-joining them is the
 * consumer's job. */
static void launch_output_append(
    struct dap_session *s, const char *text, size_t len)
{
	size_t room = DAP_SESSION_MAX_LAUNCH_OUTPUT - s->launch_output_len;
	size_t want;
	char *grown;

	if (len > room) {
		len = room;
		s->launch_output_truncated = true;
	}
	if (len == 0) {
		return;
	}
	if (s->launch_output_len + len > s->launch_output_cap) {
		want = s->launch_output_cap ? s->launch_output_cap * 2 : 4096;
		while (want < s->launch_output_len + len) {
			want *= 2;
		}
		grown = realloc(s->launch_output, want);
		if (!grown) {
			s->launch_output_truncated = true;
			return;
		}
		s->launch_output = grown;
		s->launch_output_cap = want;
	}
	memcpy(s->launch_output + s->launch_output_len, text, len);
	s->launch_output_len += len;
}

static void launch_output_release(struct dap_session *s)
{
	free(s->launch_output);
	s->launch_output = NULL;
	s->launch_output_len = 0;
	s->launch_output_cap = 0;
	s->launch_output_truncated = false;
}

static void on_output(
    void *ctx, const char *category, const char *text, size_t len)
{
	struct dap_session *s = ctx;

	if (!s->st.launch_done && category && strcmp(category, "stderr") == 0) {
		launch_output_append(s, text, len);
	}
	if (s->hooks.output) {
		s->hooks.output(s->hooks.ctx, category, text, len);
	}
}

static void on_log(void *ctx, enum dap_log_level level, const char *text)
{
	struct dap_session *s = ctx;

	if (level == DAP_LOG_ERROR) {
		session_report(s, true, "%s", text);
	}
}

/* ------------------------------- teardown ----------------------------- */

static void begin_close(struct dap_session *s)
{
	if (!s->client) {
		become_dead(s, "the debug session ended");
		return;
	}
	/* The ladder: flush what is queued, half-close, wait out the grace
	 * period, then SIGTERM and SIGKILL a child this transport owns --
	 * and nothing at all on a kind that owns none. */
	(void)dap_transport_begin_shutdown(dap_client_transport(s->client));
	s->closing = true;
}

/* The answer to `disconnect` or `terminate`, and the only thing either one
 * leads to.  A timed-out `terminate` closes exactly as an answered one
 * does: it is forceful user-requested termination, not a protocol failure,
 * and it never turns into a second request. */
static void on_end_response(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	struct dap_session *s = ctx;

	(void)c;
	if (!r->answered) {
		session_report(s, false,
		    "the adapter did not answer %s; closing it", r->command);
	}
	begin_close(s);
}

static void send_disconnect(struct dap_session *s, bool terminate_debuggee)
{
	struct kg_jsonw w;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "restart");
	kg_jsonw_bool(&w, false);
	kg_jsonw_key(&w, "terminateDebuggee");
	kg_jsonw_bool(&w, terminate_debuggee);
	kg_jsonw_end_object(&w);
	send_built(s, "disconnect", &w, on_end_response, s);
}

static void send_terminate(struct dap_session *s)
{
	struct kg_jsonw w;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "restart");
	kg_jsonw_bool(&w, false);
	kg_jsonw_end_object(&w);
	send_built(s, "terminate", &w, on_end_response, s);
}

void dap_session_end(struct dap_session *s, enum dap_end_request request)
{
	if (!s || s->st.phase == DAP_PHASE_DEAD || s->ending || s->dying) {
		return;
	}
	s->ending = true;
	s->st.end_request = request;
	s->st.phase = DAP_PHASE_ENDING;
	s->st.end_action = dap_session_end_action(request, s->st.attached,
	    session_capable(s, DAP_CAP_TERMINATE_REQUEST),
	    session_capable(s, DAP_CAP_TERMINATE_DEBUGGEE));
	session_changed(s);
	switch (s->st.end_action) {
	case DAP_END_ACTION_CLOSE:
		begin_close(s);
		return;
	case DAP_END_ACTION_DISCONNECT_KEEP:
		send_disconnect(s, false);
		return;
	case DAP_END_ACTION_DISCONNECT_TERMINATE:
		send_disconnect(s, true);
		return;
	case DAP_END_ACTION_TERMINATE:
		send_terminate(s);
		return;
	}
}

void dap_session_cancel(struct dap_session *s)
{
	if (!s || s->st.phase == DAP_PHASE_DEAD) {
		return;
	}
	/* Closing the transport under the conversation is the whole of
	 * cancelling: a launch that has no deadline ends because its
	 * transport did, and every pending callback fails once on the way. */
	s->ending = true;
	s->st.end_request = DAP_END_CLEANUP;
	s->st.end_action = DAP_END_ACTION_CLOSE;
	become_dead(s, "the debug session was cancelled");
}

/* ------------------------------- the registry ------------------------- */

static bool registry_add(struct dap_session *s)
{
	size_t i;

	for (i = 0; i < DAP_SESSION_MAX_SESSIONS; i++) {
		if (!g_sessions[i]) {
			g_sessions[i] = s;
			return true;
		}
	}
	return false;
}

static void registry_remove(const struct dap_session *s)
{
	size_t i;

	for (i = 0; i < DAP_SESSION_MAX_SESSIONS; i++) {
		if (g_sessions[i] == s) {
			g_sessions[i] = NULL;
		}
	}
}

struct dap_session *dap_session_current(void) { return g_sessions[0]; }

/* --------------------------------- start ------------------------------ */

static bool directory_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static struct dap_transport *start_transport(
    const struct dap_adapter_spec *spec, const unsigned char *secret,
    size_t secret_len, char *error, size_t error_size)
{
	const char *argv[DAP_CONFIG_MAX_ARGV + 1];
	struct kg_spawn_request req = { .stdin_fd = -1 };
	struct dap_transport *t;

	if (spec->transport == DAP_TRANSPORT_LSP_SIBLING) {
		/* No command, and no child: the adapter is a socket some
		 * other process is listening on, and the secret in front of
		 * the first frame is what it wants instead of a handshake of
		 * its own.  Both the address and the secret were resolved by
		 * whoever called dap_session_start_sibling(). */
		t = dap_transport_connect_secret(
		    spec->host, spec->port, secret, secret_len);
		if (!t) {
			snprintf(error, error_size,
			    "could not reach the adapter at %s:%u", spec->host,
			    (unsigned)spec->port);
		}
		return t;
	}
	if (spec->transport == DAP_TRANSPORT_TCP_ATTACH) {
		t = dap_transport_attach_tcp(spec->host, spec->port);
		if (!t) {
			snprintf(error, error_size,
			    "could not reach the adapter at %s:%u", spec->host,
			    (unsigned)spec->port);
		}
		return t;
	}
	if (spec->command[0]) {
		req.command = spec->command;
	} else if (dap_adapter_spec_argv(
		       spec, argv, sizeof(argv) / sizeof(*argv))
	    > 0) {
		req.argv = argv;
	} else {
		snprintf(error, error_size, "the adapter `%s` has no command",
		    spec->name);
		return NULL;
	}
	if (spec->cwd[0]) {
		/* Resolved BEFORE the spawn, and required to exist.  It is the
		 * directory the adapter itself runs in, which for delve is
		 * what `program` and its build artefact are relative to
		 * (measured, doc/plans/dap/04-go.md) -- so a missing one is a
		 * debugger that would build and debug the wrong thing rather
		 * than an inconvenience to report later. */
		if (!directory_exists(spec->cwd)) {
			snprintf(error, error_size,
			    "the adapter `%s` needs to run in `%s`, which is "
			    "not a directory",
			    spec->name, spec->cwd);
			return NULL;
		}
		req.directory = spec->cwd;
	}
	t = spec->transport == DAP_TRANSPORT_SPAWN_PORT
	    ? dap_transport_start_spawn_port(&req, spec->announce_prefix)
	    : dap_transport_start_stdio(&req);
	if (!t) {
		snprintf(error, error_size, "could not start `%s`: %s",
		    spec->name, strerror(errno));
	}
	return t;
}

/* The adapter's command line as one string, for the failure that has to
 * name it.  Bounded and lossy on purpose: it is a message, not an argv. */
static void spec_command_text(
    const struct dap_adapter_spec *spec, char *out, size_t out_size)
{
	const char *argv[DAP_CONFIG_MAX_ARGV + 1];
	size_t count = dap_adapter_spec_argv(spec, argv, ARRAY_LEN(argv));
	size_t used = 0;
	size_t i;

	if (spec->command[0]) {
		/* Deliberately lossy, and told to the compiler as such: an
		 * adapter command line is up to a page and this is a
		 * sentence. */
		snprintf(
		    out, out_size, "%.*s", (int)out_size - 1, spec->command);
		return;
	}
	out[0] = '\0';
	for (i = 0; i < count && used + 1 < out_size; i++) {
		used += (size_t)snprintf(
		    out + used, out_size - used, "%s%s", i ? " " : "", argv[i]);
	}
}

static struct dap_session *session_start(
    const struct dap_session_request *request, const unsigned char *secret,
    size_t secret_len, char *error, size_t error_size)
{
	struct dap_client_hooks client_hooks
	    = { NULL, on_event, on_output, on_log };
	struct dap_transport *t;
	struct dap_session *s;

	if (error && error_size) {
		error[0] = '\0';
	}
	if (dap_session_current()) {
		snprintf(
		    error, error_size, "a debug session is already running");
		return NULL;
	}
	t = start_transport(
	    request->adapter, secret, secret_len, error, error_size);
	if (!t) {
		return NULL;
	}
	s = calloc(1, sizeof(*s));
	if (s) {
		s->arguments = malloc(request->arguments_len + 1);
	}
	if (!s || !s->arguments) {
		free(s);
		dap_transport_close(t);
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	memcpy(s->arguments, request->arguments, request->arguments_len);
	s->arguments[request->arguments_len] = '\0';
	s->arguments_len = request->arguments_len;
	s->request = request->request;
	s->st.attached = request->request == DAP_REQUEST_ATTACH;
	if (request->hooks) {
		s->hooks = *request->hooks;
	}
	snprintf(s->adapter_id, sizeof(s->adapter_id), "%s",
	    request->adapter->adapter_id);
	snprintf(s->config_name, sizeof(s->config_name), "%s",
	    request->config_name ? request->config_name : "");
	snprintf(s->adapter_cwd, sizeof(s->adapter_cwd), "%s",
	    request->adapter->cwd);
	s->client = dap_client_new(t);
	if (!s->client) {
		free(s->arguments);
		free(s);
		snprintf(error, error_size, "out of memory");
		return NULL;
	}
	client_hooks.ctx = s;
	dap_client_set_hooks(s->client, &client_hooks);
	{
		char command[DAP_SESSION_TEXT_MAX];

		spec_command_text(request->adapter, command, sizeof(command));
		dap_client_set_adapter_command(s->client, command);
	}
	dap_client_set_adapter_stderr_to_output(
	    s->client, request->adapter->route_stderr_to_output);
	/* Registered BEFORE the handshake: initialize can fail synchronously,
	 * and a session that ended inside its own start must still have been
	 * the current one while it did. */
	(void)registry_add(s);
	(void)dap_client_initialize(s->client, s->adapter_id, on_initialize, s);
	return s;
}

struct dap_session *dap_session_start(
    const struct dap_session_request *request, char *error, size_t error_size)
{
	return session_start(request, NULL, 0, error, error_size);
}

struct dap_session *dap_session_start_sibling(
    const struct dap_session_request *request, const unsigned char *secret,
    size_t secret_len, char *error, size_t error_size)
{
	return session_start(request, secret, secret_len, error, error_size);
}

/* --------------------------------- polling ---------------------------- */

int dap_session_poll(struct dap_session *s)
{
	struct dap_transport *t;
	int changed = 0;

	if (!s || s->st.phase == DAP_PHASE_DEAD || !s->client) {
		return 0;
	}
	changed |= dap_client_poll(s->client) != 0;
	if (s->st.phase == DAP_PHASE_DEAD) {
		return 1; /* a callback ended it from inside the poll */
	}
	if (s->closing && s->client) {
		t = dap_client_transport(s->client);
		/* The ladder waiting out a grace period is not news: only its
		 * end is, so an ending session does not ask for a repaint on
		 * every idle tick until it is gone. */
		if (t && dap_transport_shutdown_poll(t) == 0) {
			become_dead(s, "the debug session ended");
			return 1;
		}
		return changed;
	}
	if (!s->client || !dap_client_alive(s->client)) {
		/* End of stream is an ending like any other [M-8]: an adapter
		 * that crashed looks exactly like this. */
		become_dead(s, dap_client_death_text(s->client));
		return 1;
	}
	return changed;
}

int dap_session_poll_all(void)
{
	int changed = 0;
	size_t i;

	for (i = 0; i < DAP_SESSION_MAX_SESSIONS; i++) {
		changed |= dap_session_poll(g_sessions[i]);
	}
	return changed;
}

int dap_session_wait_fds(int *fds, int max)
{
	int count = 0;
	size_t i;

	for (i = 0; i < DAP_SESSION_MAX_SESSIONS && count < max; i++) {
		if (g_sessions[i] && g_sessions[i]->client) {
			count += dap_client_wait_fds(
			    g_sessions[i]->client, fds + count, max - count);
		}
	}
	return count;
}

/* ------------------------------- shutdown ----------------------------- */

static double monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static bool any_session_alive(void)
{
	size_t i;

	for (i = 0; i < DAP_SESSION_MAX_SESSIONS; i++) {
		if (g_sessions[i]
		    && g_sessions[i]->st.phase != DAP_PHASE_DEAD) {
			return true;
		}
	}
	return false;
}

/* The editor is exiting: every session ends with DAP_END_CLEANUP, which
 * terminates a launched debuggee and detaches an attached one, and the
 * whole budget is `grace_ms` for all of them together.  A session that
 * wants longer does not get it -- the transport's kill backstop is what
 * collects an adapter kg started. */
void dap_session_shutdown_all(unsigned grace_ms)
{
	struct timespec nap = { 0, 1000000 };
	double deadline;
	size_t i;

	for (i = 0; i < DAP_SESSION_MAX_SESSIONS; i++) {
		if (g_sessions[i]) {
			struct dap_transport_deadlines limits
			    = { DAP_TRANSPORT_SPAWN_TIMEOUT_MS,
				      DAP_TRANSPORT_CONNECT_TIMEOUT_MS,
				      grace_ms / 2, grace_ms / 2 };

			if (g_sessions[i]->client) {
				dap_transport_set_deadlines(
				    dap_client_transport(g_sessions[i]->client),
				    &limits);
			}
			dap_session_end(g_sessions[i], DAP_END_CLEANUP);
		}
	}
	deadline = monotonic_ms() + grace_ms;
	while (any_session_alive() && monotonic_ms() < deadline) {
		(void)dap_session_poll_all();
		nanosleep(&nap, NULL);
	}
	for (i = 0; i < DAP_SESSION_MAX_SESSIONS; i++) {
		dap_session_close(g_sessions[i]);
	}
}

void dap_session_close(struct dap_session *s)
{
	if (!s) {
		return;
	}
	become_dead(s, "the debug session ended");
	registry_remove(s);
	launch_output_release(s);
	free(s->arguments);
	free(s->sources);
	free(s->slots);
	free(s);
}

/* ------------------------------- accessors ---------------------------- */

void dap_session_get_state(
    const struct dap_session *s, struct dap_session_state *out)
{
	if (!s) {
		memset(out, 0, sizeof(*out));
		out->phase = DAP_PHASE_DEAD;
		return;
	}
	*out = s->st;
}

struct dap_client *dap_session_client(struct dap_session *s)
{
	return s ? s->client : NULL;
}

void dap_session_set_timeouts(
    struct dap_session *s, unsigned initialize_ms, unsigned request_ms)
{
	if (s && s->client) {
		dap_client_set_timeouts(s->client, initialize_ms, request_ms);
	}
}

void dap_session_set_spawn_deadline(struct dap_session *s, unsigned spawn_ms)
{
	struct dap_transport_deadlines limits
	    = { spawn_ms, DAP_TRANSPORT_CONNECT_TIMEOUT_MS,
		      DAP_TRANSPORT_SHUTDOWN_TIMEOUT_MS,
		      DAP_TRANSPORT_KILL_TIMEOUT_MS };

	if (s && s->client) {
		dap_transport_set_deadlines(
		    dap_client_transport(s->client), &limits);
	}
}

void dap_session_set_shutdown_deadlines(
    struct dap_session *s, unsigned shutdown_ms, unsigned kill_ms)
{
	struct dap_transport_deadlines limits
	    = { DAP_TRANSPORT_SPAWN_TIMEOUT_MS,
		      DAP_TRANSPORT_CONNECT_TIMEOUT_MS, shutdown_ms, kill_ms };

	if (s && s->client) {
		dap_transport_set_deadlines(
		    dap_client_transport(s->client), &limits);
	}
}
