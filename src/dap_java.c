/* nbcode's half of Java debugging: the endpoint the language server
 * announces, and the teardown it never announces.  See src/dap_java.h. */

#include "dap_java.h"

#include "dap_config.h"
#include "dap_exec.h"
#include "dap_session.h"
#include "dap_transport.h"
#include "json.h"
#include "lsp.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The starting sequence, and the running session's inference, in one
 * record.  ONE of them, because v1 has one session
 * (DAP_SESSION_MAX_SESSIONS), and the field the multi-session change would
 * key on is `session` -- which is already here. */
struct java_state {
	enum dap_java_step step;
	/* What dap_java_start() was handed, copied: the caller frees its
	 * arguments the moment it returns. */
	struct dap_adapter_spec spec;
	struct dap_session_hooks hooks;
	enum dap_request_kind request;
	char *arguments;
	size_t arguments_len;
	char config_name[DAP_CONFIG_NAME_MAX];
	char filename[PATH_MAX];
	/* The generation of the language-server child whose endpoint this
	 * session's socket is connected to.  A different one means the owner
	 * was replaced, which makes this session's endpoint stale. */
	uint64_t generation;
	struct dap_session *session;
	/* The teardown inference.  `exited_seen` is what makes it
	 * conservative: no thread has ended, so nothing has finished, so
	 * there is nothing to infer however few threads are live. */
	bool exited_seen;
	bool grace_running;
	int64_t grace_started_ms;
	unsigned grace_ms;
	char status[DAP_SESSION_TEXT_MAX];
};

static struct java_state g_java;
static bool g_grace_ms_set;
static unsigned g_grace_ms = DAP_JAVA_TEARDOWN_GRACE_MS;
static void (*g_session_hook)(void);

void dap_java_set_session_hook(void (*fn)(void)) { g_session_hook = fn; }

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

void dap_java_set_grace_ms(unsigned grace_ms)
{
	g_grace_ms = grace_ms;
	g_grace_ms_set = true;
	g_java.grace_ms = grace_ms;
}

bool dap_java_owns(const struct dap_adapter_spec *spec)
{
	return spec && spec->transport == DAP_TRANSPORT_LSP_SIBLING;
}

static void state_clear(void)
{
	free(g_java.arguments);
	memset(&g_java, 0, sizeof(g_java));
	g_java.grace_ms
	    = g_grace_ms_set ? g_grace_ms : DAP_JAVA_TEARDOWN_GRACE_MS;
}

/* `path` under kg's own working directory when it is not already
 * absolute.  No realpath(): the file may be one the user has named but not
 * created, and an absolute-but-uncanonical directory is a root kg and the
 * language server still agree on (src/lsp_server.c makes the same
 * choice). */
static bool absolute_path(const char *path, char *out, size_t out_size)
{
	char cwd[PATH_MAX];
	int wrote;

	if (path[0] == '/') {
		return (size_t)snprintf(out, out_size, "%s", path) < out_size;
	}
	if (!getcwd(cwd, sizeof(cwd))) {
		return false;
	}
	wrote = snprintf(out, out_size, "%s/%s", cwd, path);
	return wrote >= 0 && (size_t)wrote < out_size;
}

static void report(bool error, const char *text)
{
	snprintf(g_java.status, sizeof(g_java.status), "%s", text);
	if (g_java.hooks.report) {
		g_java.hooks.report(g_java.hooks.ctx, error, text);
	}
}

/* --------------------------- the start sequence ----------------------- */

int dap_java_start(const struct dap_session_request *request,
    const char *filename, char *error, size_t error_size)
{
	if (error && error_size) {
		error[0] = '\0';
	}
	if (g_java.step != DAP_JAVA_IDLE) {
		snprintf(
		    error, error_size, "a debug session is already starting");
		return -1;
	}
	if (!filename || !filename[0]) {
		snprintf(error, error_size,
		    "the Java debugger needs a saved file to debug");
		return -1;
	}
	state_clear();
	/* The workspace-root walk wants an absolute path, and a buffer's own
	 * filename is only absolute if the user typed one -- `kg Fixture.java`
	 * visits a real saved file under a relative name.  A relative one
	 * means "under kg's working directory", which is what this makes it
	 * say. */
	if (!absolute_path(
		filename, g_java.filename, sizeof(g_java.filename))) {
		snprintf(error, error_size,
		    "the Java debugger cannot resolve `%s`", filename);
		return -1;
	}
	g_java.arguments = malloc(request->arguments_len + 1);
	if (!g_java.arguments) {
		snprintf(error, error_size, "out of memory");
		return -1;
	}
	memcpy(g_java.arguments, request->arguments, request->arguments_len);
	g_java.arguments[request->arguments_len] = '\0';
	g_java.arguments_len = request->arguments_len;
	g_java.spec = *request->adapter;
	g_java.request = request->request;
	if (request->hooks) {
		g_java.hooks = *request->hooks;
	}
	snprintf(g_java.config_name, sizeof(g_java.config_name), "%s",
	    request->config_name ? request->config_name : "");
	g_java.step = DAP_JAVA_RESOLVING;
	report(false, "Java: finding the language server");
	return 0;
}

/* The endpoint is in hand: fill it into the spec and start an ordinary
 * session on it.  The secret never leaves this function except into the
 * transport's outbox -- it is not stored on the spec, not logged, and not
 * part of any message a user sees. */
static int session_begin(const struct kg_lsp_sibling_endpoint *endpoint)
{
	struct dap_session_request request
	    = { &g_java.spec, g_java.request, g_java.arguments,
		      g_java.arguments_len, g_java.config_name, &g_java.hooks };
	char error[DAP_SESSION_TEXT_MAX] = "";

	snprintf(
	    g_java.spec.host, sizeof(g_java.spec.host), "%s", endpoint->host);
	g_java.spec.port = endpoint->port;
	g_java.generation = endpoint->generation;
	g_java.session = dap_session_start_sibling(&request, endpoint->secret,
	    endpoint->secret_len, error, sizeof(error));
	if (!g_java.session) {
		report(true,
		    error[0] ? error
			     : "could not reach the Java debug "
			       "adapter");
		state_clear();
		return -1;
	}
	g_java.step = DAP_JAVA_RUNNING;
	/* The stop model is told here rather than by the command layer,
	 * because here is where the session came into existence -- the
	 * command layer returned seconds ago. */
	dap_exec_session_started();
	if (g_session_hook) {
		g_session_hook();
	}
	report(false, "Java: connected to the debug adapter");
	return 0;
}

/* An endpoint that did not come from a language server.  See
 * dap_java_set_endpoint_for_test(): the ONE thing a suite with no nbcode,
 * no JVM and no Java project cannot otherwise reach is this module's
 * RUNNING step, and everything the teardown inference does lives there. */
static bool g_test_endpoint_set;
static struct kg_lsp_sibling_endpoint g_test_endpoint;

void dap_java_set_endpoint_for_test(
    const char *host, unsigned short port, const char *secret)
{
	memset(&g_test_endpoint, 0, sizeof(g_test_endpoint));
	g_test_endpoint_set = host != NULL;
	if (!g_test_endpoint_set) {
		return;
	}
	snprintf(
	    g_test_endpoint.host, sizeof(g_test_endpoint.host), "%s", host);
	g_test_endpoint.port = port;
	g_test_endpoint.secret_len
	    = strnlen(secret, sizeof(g_test_endpoint.secret));
	memcpy(g_test_endpoint.secret, secret, g_test_endpoint.secret_len);
	/* One generation, unchanging, so the owner check below says the
	 * server that announced this endpoint is still the one running. */
	g_test_endpoint.generation = 1;
}

/* Where the endpoint comes from, asked in one place because two ask: the
 * resolve step and the owner check, with different intents. */
static enum kg_lsp_sibling_status ask_endpoint(
    enum kg_lsp_sibling_intent intent, struct kg_lsp_sibling_endpoint *out)
{
	if (g_test_endpoint_set) {
		*out = g_test_endpoint;
		return KG_LSP_SIBLING_OK;
	}
	return lsp_sibling_endpoint(
	    g_java.spec.lsp_language, g_java.filename, intent, out);
}

/* One turn of the resolve step.  Every status but OK either keeps waiting
 * or ends the attempt, and the two are told apart by whether asking again
 * can change the answer. */
static int resolve_advance(void)
{
	struct kg_lsp_sibling_endpoint endpoint;
	enum kg_lsp_sibling_status status
	    = ask_endpoint(KG_LSP_SIBLING_START, &endpoint);

	switch (status) {
	case KG_LSP_SIBLING_OK:
		return session_begin(&endpoint) == 0;
	case KG_LSP_SIBLING_STARTING:
		report(false, "Java: waiting for the language server");
		return 0;
	case KG_LSP_SIBLING_NONE:
		report(false, "Java: waiting for the debug adapter's port");
		return 0;
	case KG_LSP_SIBLING_UNSUPPORTED:
		/* The precise message the plan asks for, and the one a user
		 * with KG_LSP_SERVER_JAVA=jdtls has to see: the debugger is
		 * not missing, the SERVER is the wrong one. */
		report(true,
		    "Java debugging requires the nbcode Java server "
		    "(see KG_LSP_SERVER_JAVA)");
		state_clear();
		return 1;
	case KG_LSP_SIBLING_NOT_BUILT:
	case KG_LSP_SIBLING_NOT_RUNNING:
	case KG_LSP_SIBLING_DEAD:
		report(true, lsp_sibling_status_text(status));
		state_clear();
		return 1;
	}
	report(true, "Java: the language server gave an answer kg cannot read");
	state_clear();
	return 1;
}

/* ------------------------- the teardown inference --------------------- */

/* The JVM's own threads, which are alive in every `threads` answer nbcode
 * gives -- before the debuggee starts, while it runs, and for as long as
 * the adapter is willing to answer after it has finished.
 *
 * MEASURED, 2026-08-12, oracle-java 26.0.1 on a single-file program with no
 * breakpoints: the program's own thread is announced `started` and then
 * `exited`, and `threads` from that moment on answers exactly
 * {Signal Dispatcher, Reference Handler, Finalizer, Notification Thread,
 * Common-Cleaner} -- five, forever.  The plan wrote this step as "when no
 * live debuggee thread remains", and taking that literally as an EMPTY
 * thread list would be a session that hangs until the user kills it, since
 * the list never empties.
 *
 * So "debuggee thread" is a thread that is not one of these.  It is a
 * shabby discriminator and it is deliberately here rather than one layer
 * down, where it would become the protocol's opinion about what a thread
 * is.  Two things keep the shabbiness safe: the inference needs a thread
 * to have actually EXITED before it will fire at all, and anything that
 * proves the session is alive -- a new thread, a stop -- cancels it.  The
 * cost of being wrong is a session disconnected early, and the grace period
 * is what makes that unlikely rather than the list being exhaustive. */
static const char *const jvm_system_threads[] = {
	"Signal Dispatcher",
	"Reference Handler",
	"Finalizer",
	"Notification Thread",
	"Common-Cleaner",
	/* Not in the measured five, and here because they are the same kind
	 * of thing and cost nothing: a JVM that reports them instead would
	 * otherwise never let the inference fire. */
	"Attach Listener",
	"process reaper",
};

static bool thread_is_jvm_internal(const char *name)
{
	size_t i;

	for (i = 0;
	    i < sizeof(jvm_system_threads) / sizeof(*jvm_system_threads); i++) {
		if (strcmp(name, jvm_system_threads[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* Whether any thread the execution model currently knows about belongs to
 * the program being debugged.  The model's list is the refreshed one: a
 * `thread` event triggers its debounced `threads` re-request, so this is
 * asked of what the adapter last said rather than of an event tally, which
 * is the only version that survives nbcode announcing one thread `started`
 * twice (measured). */
static bool debuggee_thread_alive(void)
{
	struct dap_exec_thread thread;
	size_t i;

	for (i = 0; i < dap_exec_thread_count(); i++) {
		if (dap_exec_thread(i, &thread)
		    && !thread_is_jvm_internal(thread.name)) {
			return true;
		}
	}
	return false;
}

/* Anything that proves the session is alive cancels a running inference. */
static void grace_cancel(void)
{
	if (g_java.grace_running) {
		g_java.grace_running = false;
	}
}

static void grace_start(void)
{
	if (g_java.grace_running) {
		return;
	}
	g_java.grace_running = true;
	g_java.grace_started_ms = monotonic_ms();
}

void dap_java_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	const char *reason;

	/* The model first and unconditionally: this layer adds an opinion,
	 * it does not stand between the adapter and the state machine. */
	dap_exec_event(ctx, event, body);
	if (g_java.step != DAP_JAVA_RUNNING || !event) {
		return;
	}
	if (strcmp(event, "stopped") == 0) {
		/* A stop is the loudest possible proof of life, and it is
		 * also how a session with breakpoints spends most of its
		 * time.  Cancel outright. */
		grace_cancel();
		return;
	}
	if (strcmp(event, "thread") != 0) {
		return;
	}
	reason = kg_json_str(kg_json_get(body, "reason"), NULL);
	if (reason && strcmp(reason, "exited") == 0) {
		g_java.exited_seen = true;
		return;
	}
	/* A thread STARTED: the program is doing something, whatever the
	 * list said a moment ago. */
	grace_cancel();
}

void dap_java_session_ended(void) { state_clear(); }

/* --------------------------------- polling ---------------------------- */

/* The owner check.  Asked of the EXISTING instance so that a language
 * server which has died is reported dead rather than restarted: a fresh
 * nbcode has a fresh port, and this session's socket is connected to the
 * old one.  A changed generation is the same news reached the other way. */
static bool owner_still_ours(void)
{
	struct kg_lsp_sibling_endpoint endpoint;
	enum kg_lsp_sibling_status status
	    = ask_endpoint(KG_LSP_SIBLING_EXISTING, &endpoint);

	if (status == KG_LSP_SIBLING_DEAD
	    || status == KG_LSP_SIBLING_NOT_RUNNING) {
		return false;
	}
	/* Every other status -- STARTING, NONE, UNSUPPORTED -- means the
	 * owner is there and merely has nothing new to say.  Only a
	 * generation that CHANGED proves it was replaced. */
	return status != KG_LSP_SIBLING_OK
	    || endpoint.generation == g_java.generation;
}

static int running_poll(void)
{
	if (!g_java.session || dap_session_current() != g_java.session) {
		/* The session ended by some other route -- the user
		 * disconnected, the transport failed, the editor is exiting.
		 * Nothing here has an opinion about that. */
		state_clear();
		return 0;
	}
	if (!owner_still_ours()) {
		report(true,
		    "Java: the language server that owns the debug adapter is "
		    "gone");
		/* Its socket is connected to a process that is not there.
		 * There is nobody to say `disconnect` to, so the session ends
		 * the way a dead transport ends one -- and the child is NOT
		 * this layer's to kill, now less than ever. */
		dap_session_end(g_java.session, DAP_END_LAUNCH_FAILURE);
		state_clear();
		return 1;
	}
	if (!g_java.exited_seen) {
		return 0;
	}
	if (debuggee_thread_alive()) {
		grace_cancel();
		return 0;
	}
	grace_start();
	if (monotonic_ms() - g_java.grace_started_ms
	    < (int64_t)g_java.grace_ms) {
		return 0;
	}
	/* nbcode will never say `terminated` (measured to 60 s past program
	 * completion), so this is the end of the session -- and it ends with
	 * a `disconnect` that keeps the debuggee, which closes ONE SOCKET.
	 * The language server this adapter lives in is the user's and stays
	 * running. */
	report(false, "Java: the program has finished");
	dap_session_end(g_java.session, DAP_END_DISCONNECT);
	state_clear();
	return 1;
}

int dap_java_poll(void)
{
	switch (g_java.step) {
	case DAP_JAVA_IDLE:
		return 0;
	case DAP_JAVA_RESOLVING:
		return resolve_advance();
	case DAP_JAVA_RUNNING:
		return running_poll();
	}
	return 0;
}

void dap_java_cancel(void)
{
	if (g_java.step == DAP_JAVA_RESOLVING) {
		report(false, "Java: cancelled");
		state_clear();
	}
}

const char *dap_java_step_text(void)
{
	switch (g_java.step) {
	case DAP_JAVA_IDLE:
		return NULL;
	case DAP_JAVA_RESOLVING:
	case DAP_JAVA_RUNNING:
		return g_java.status;
	}
	return NULL;
}

enum dap_java_step dap_java_step(void) { return g_java.step; }
