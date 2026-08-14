#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_dap_java.c -- nbcode's half of Java debugging (src/dap_java.c and
 * the seams it needs, subplan doc/plans/dap/03-java.md).
 *
 * Three things are under test here and they are deliberately in one file,
 * because the property that matters is how they compose:
 *
 *   the lsp-sibling WIRE   -- a socket somebody else is listening on, with
 *                             a secret in front of the first frame;
 *   the ENDPOINT seam      -- the debugger asking the LSP layer where that
 *                             socket is, and being told when it is stale;
 *   the TEARDOWN inference -- what a client does about an adapter that
 *                             never says `terminated`.
 *
 * The wire cases drive a real child (test/fake_dap_adapter.py with
 * `--listen-secret`) over a real loopback socket, because a partial write
 * of the secret and an `initialize` queued behind it are not reachable with
 * one process.  The inference cases drive the state machine directly: what
 * they assert is a POLICY about events, and manufacturing a JVM to produce
 * them would test the JVM.  The ones that need a RUNNING session put the
 * same fake behind the same wire and hand the start sequence its address
 * (dap_java_set_endpoint_for_test), so what is faked is where the endpoint
 * came from and nothing else -- the session, the socket, the handshake and
 * the thread list are all real.
 *
 * Nothing sleeps blind: every wait for something to HAPPEN is a bounded pump
 * loop over a predicate.  The only fixed sleeps are in the inference cases,
 * where what has to pass is a window rather than an event -- and the window
 * is the thing under test, shortened to a fifth of a second for the purpose.
 */

#include "../src/dap_breakpoint.h"
#include "../src/dap_client.h"
#include "../src/dap_config.h"
#include "../src/dap_exec.h"
#include "../src/dap_java.h"
#include "../src/dap_session.h"
#include "../src/dap_transport.h"
#include "../src/json.h"
#include "../src/lsp.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PUMP_DEADLINE_SECONDS 10.0

/* The secret the sibling cases hand over.  128 lowercase hex characters,
 * which is what a real nbcode announces -- the length matters because a
 * short one would not exercise the partial-write path this file is about. */
#define SIBLING_SECRET                                                         \
	"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"     \
	"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

static char script_path[512];

static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap_a_millisecond(void)
{
	struct timespec nap = { 0, 1000000 };

	nanosleep(&nap, NULL);
}

/* ------------------------------- the wire ----------------------------- */

/* Start the fake listening for a sibling connection and read the port it
 * announces on its own standard output.  Returns 0 on failure, which every
 * caller CHECKs before using -- gcc's -fanalyzer runs on the tests, and a
 * CHECK() records and carries on rather than stopping. */
static unsigned short listen_fake(
    FILE **child, const char *secret, const char *extra)
{
	char command[2048];
	char line[128];
	unsigned port = 0;

	snprintf(command, sizeof(command),
	    "python3 '%s' --mode protocol --listen-secret '%s' "
	    "--capabilities '{\"supportsConfigurationDoneRequest\":true}' %s",
	    script_path, secret, extra ? extra : "");
	*child = popen(command, "r");
	if (!*child) {
		return 0;
	}
	if (!fgets(line, sizeof(line), *child)
	    || sscanf(line, "PORT %u", &port) != 1 || port == 0
	    || port > 65535) {
		return 0;
	}
	return (unsigned short)port;
}

/* The whole of the lsp-sibling connect, end to end and through a real
 * socket: the transport connects, writes the secret with no newline and
 * nothing framing it, and the `initialize` queued immediately afterwards
 * arrives BEHIND it.  The fake refuses -- closes the socket without a word,
 * as nbcode does -- if either half of that is wrong, so an answered
 * request is the assertion. */
static void test_sibling_connect_writes_the_secret_first(void)
{
	struct dap_transport *t;
	FILE *child = NULL;
	unsigned short port = listen_fake(&child, SIBLING_SECRET, NULL);
	const char *body = NULL;
	size_t len = 0;
	double deadline;
	bool answered = false;
	static const char request[]
	    = "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\","
	      "\"arguments\":{\"adapterID\":\"nbcode-java\"}}";

	CHECK(port != 0);
	if (!port) {
		if (child) {
			pclose(child);
		}
		return;
	}
	t = dap_transport_connect_secret("127.0.0.1", port,
	    (const unsigned char *)SIBLING_SECRET, strlen(SIBLING_SECRET));
	CHECK(t != NULL);
	if (!t) {
		pclose(child);
		return;
	}
	/* Queued while the connect may still be in progress, which is the
	 * ordering the prepend hook exists for. */
	CHECK(dap_transport_send(t, request, sizeof(request) - 1) == 0);
	deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	while (monotonic_seconds() < deadline && !answered) {
		(void)dap_transport_flush(t);
		if (dap_transport_next_message(t, &body, &len) == 1) {
			answered = true;
			break;
		}
		nap_a_millisecond();
	}
	CHECKF(answered, "the adapter never answered through the handshake");
	/* No child of its own, ever: the language server owns that process,
	 * and a debug session ending must leave it running. */
	CHECK(dap_transport_kind(t) == DAP_TRANSPORT_LSP_SIBLING);
	CHECK(!dap_transport_kind_owns_child(DAP_TRANSPORT_LSP_SIBLING));
	CHECK(dap_transport_pid(t) == -1);
	CHECK(!dap_transport_child_alive(t));
	dap_transport_close(t);
	pclose(child);
}

/* The wrong secret is a socket closed without a word, which the transport
 * reports as end of stream rather than as an answer.  The case exists
 * because the failure it guards against is the opposite: a client that
 * treated "connected" as "handshaken" would report a live session to a
 * socket nobody is reading. */
static void test_sibling_connect_with_the_wrong_secret_dies(void)
{
	struct dap_transport *t;
	FILE *child = NULL;
	unsigned short port = listen_fake(&child, SIBLING_SECRET, NULL);
	const char *body = NULL;
	size_t len = 0;
	double deadline;
	/* The same LENGTH as the real one and different bytes, which is the
	 * only shape that tests the refusal: a SHORTER secret leaves the
	 * adapter waiting for bytes rather than rejecting what it got, and
	 * the case would be measuring a deadline instead. */
	static const char wrong[]
	    = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
	      "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
	      "f";
	static const char request[]
	    = "{\"seq\":1,\"type\":\"request\",\"command\":\"initialize\"}";

	CHECK(port != 0);
	if (!port) {
		if (child) {
			pclose(child);
		}
		return;
	}
	t = dap_transport_connect_secret(
	    "127.0.0.1", port, (const unsigned char *)wrong, strlen(wrong));
	CHECK(t != NULL);
	if (!t) {
		pclose(child);
		return;
	}
	(void)dap_transport_send(t, request, sizeof(request) - 1);
	deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	while (monotonic_seconds() < deadline && !dap_transport_failed(t)) {
		(void)dap_transport_flush(t);
		(void)dap_transport_next_message(t, &body, &len);
		nap_a_millisecond();
	}
	CHECK(dap_transport_failed(t));
	dap_transport_close(t);
	pclose(child);
}

/* attach_socket() owns the non-blocking guarantee, and this is the case
 * that says so.  A BLOCKING descriptor is handed in on purpose: the probe
 * this constructor came from sat in one read() for eight minutes because
 * its caller forgot, so the transport must not merely tolerate a blocking
 * descriptor, it must fix one.  The assertion is the fcntl flag and then
 * the behaviour -- a receive on a socket nobody has written to returns 0
 * rather than blocking. */
static void test_attach_socket_makes_the_descriptor_nonblocking(void)
{
	struct dap_transport *t;
	const char *body = NULL;
	size_t len = 0;
	int pair[2];
	int flags;

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		return;
	}
	/* Explicitly blocking, which is a socketpair's default anyway --
	 * stated rather than assumed, since the whole case is about who
	 * clears it. */
	flags = fcntl(pair[0], F_GETFL, 0);
	CHECK(flags >= 0 && (flags & O_NONBLOCK) == 0);
	t = dap_transport_attach_socket(pair[0]);
	CHECK(t != NULL);
	if (!t) {
		close(pair[0]);
		close(pair[1]);
		return;
	}
	flags = fcntl(pair[0], F_GETFL, 0);
	CHECKF(flags >= 0 && (flags & O_NONBLOCK) != 0,
	    "attach_socket() left the descriptor blocking");
	/* Nothing has been written, and this returns rather than waiting. */
	CHECK(dap_transport_next_message(t, &body, &len) == 0);
	CHECK(dap_transport_pid(t) == -1);
	dap_transport_close(t);
	close(pair[1]);
}

/* ----------------------------- the endpoint --------------------------- */

/* With no LSP client compiled in, the question still has an answer and it
 * is a specific one: NOT_BUILT, which the Java adapter turns into "this kg
 * was built without the language server" rather than into "no Java server
 * is running".  In a WITH_LSP=1 build the same call reaches the registry,
 * where a language nobody configured is UNSUPPORTED.
 *
 * Both halves are asserted from the same case because the CONTRACT is that
 * the vocabulary does not change between them -- which is what lets
 * src/dap_java.c have one switch rather than two. */
static void test_an_unknown_language_has_no_sibling(void)
{
	struct kg_lsp_sibling_endpoint endpoint;
	enum kg_lsp_sibling_status status = lsp_sibling_endpoint(
	    "cobol", "/nonexistent/x.cob", KG_LSP_SIBLING_START, &endpoint);

	CHECK(status == KG_LSP_SIBLING_UNSUPPORTED
	    || status == KG_LSP_SIBLING_NOT_BUILT);
	CHECK(lsp_sibling_status_text(status) != NULL);
	CHECK(lsp_sibling_status_text(status)[0] != '\0');
}

/* Every status has a sentence, including the ones only one build ever
 * produces.  A missing one would be an empty status line at the exact
 * moment a user needs to know why nothing is happening. */
static void test_every_sibling_status_has_a_sentence(void)
{
	static const enum kg_lsp_sibling_status all[] = {
		KG_LSP_SIBLING_OK,
		KG_LSP_SIBLING_NOT_BUILT,
		KG_LSP_SIBLING_UNSUPPORTED,
		KG_LSP_SIBLING_NOT_RUNNING,
		KG_LSP_SIBLING_STARTING,
		KG_LSP_SIBLING_NONE,
		KG_LSP_SIBLING_DEAD,
	};
	size_t i;

	for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		const char *text = lsp_sibling_status_text(all[i]);

		CHECK(text != NULL && text[0] != '\0');
	}
}

/* -------------------------- the start sequence ------------------------ */

static struct heard {
	int reports;
	int errors;
	char last[256];
} heard;

static void on_report(void *ctx, bool error, const char *text)
{
	(void)ctx;
	heard.reports++;
	if (error) {
		heard.errors++;
	}
	snprintf(heard.last, sizeof(heard.last), "%s", text);
}

/* The editor's own wiring (src/dap_commands.c): a Java session's events go
 * through this module first, which forwards every one of them to the model
 * and then forms nbcode's opinion about them. */
static struct dap_session_hooks java_hooks
    = { .report = on_report, .event = dap_java_event };

static int begin(
    const char *language, const char *filename, char *error, size_t error_size)
{
	struct dap_adapter_spec spec = { 0 };
	struct dap_session_request request = { &spec, DAP_REQUEST_LAUNCH, "{}",
		2, "Java (nbcode)", &java_hooks };

	memset(&heard, 0, sizeof(heard));
	spec.transport = DAP_TRANSPORT_LSP_SIBLING;
	snprintf(spec.name, sizeof(spec.name), "nbcode-java");
	snprintf(spec.adapter_id, sizeof(spec.adapter_id), "nbcode-java");
	snprintf(spec.lsp_language, sizeof(spec.lsp_language), "%s", language);
	return dap_java_start(&request, filename, error, error_size);
}

/* The adapter kind is what decides who starts a session, and the command
 * layer asks this rather than testing the transport itself. */
static void test_only_the_sibling_kind_is_javas(void)
{
	struct dap_adapter_spec spec = { 0 };

	spec.transport = DAP_TRANSPORT_STDIO;
	CHECK(!dap_java_owns(&spec));
	spec.transport = DAP_TRANSPORT_TCP_ATTACH;
	CHECK(!dap_java_owns(&spec));
	spec.transport = DAP_TRANSPORT_LSP_SIBLING;
	CHECK(dap_java_owns(&spec));
	CHECK(!dap_java_owns(NULL));
}

/* A buffer that has not been saved has no file for an adapter to open, and
 * the refusal happens before anything is started rather than after a JVM
 * has been waited for. */
static void test_an_unsaved_buffer_is_refused_up_front(void)
{
	char error[256] = "";

	CHECK(begin("java", "", error, sizeof(error)) != 0);
	CHECK(error[0] != '\0');
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	CHECK(begin("java", NULL, error, sizeof(error)) != 0);
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	/* A RELATIVE path is not that case: it is made absolute against kg's
	 * working directory, which is what `kg Fixture.java` means. */
	CHECK(begin("java", "relative/X.java", error, sizeof(error)) == 0);
	CHECK(dap_java_step() == DAP_JAVA_RESOLVING);
	dap_java_cancel();
}

/* A language whose server announces no debug adapter -- the jdt.ls override
 * exactly -- gets the PRECISE message, not a generic failure: the debugger
 * is not missing, the server is the wrong one.  It is reported on the first
 * poll and the sequence ends there rather than waiting for an announce that
 * will never come. */
static void test_a_language_with_no_sibling_says_which_server_it_wants(void)
{
	char error[256] = "";

	CHECK(begin("cobol", "/nonexistent/x.cob", error, sizeof(error)) == 0);
	CHECK(dap_java_step() == DAP_JAVA_RESOLVING);
	(void)dap_java_poll();
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	CHECK(heard.errors == 1);
	CHECKF(strstr(heard.last, "nbcode") != NULL
		|| strstr(heard.last, "without the language server") != NULL,
	    "the refusal did not name the server: %s", heard.last);
}

/* The wait is cancellable at the step it spends its time in, which is the
 * point of having named steps at all: a language server importing a large
 * project can hold RESOLVING for tens of seconds. */
static void test_a_waiting_sequence_can_be_cancelled(void)
{
	char error[256] = "";

	CHECK(begin("java", "/nonexistent/Fixture.java", error, sizeof(error))
	    == 0);
	CHECK(dap_java_step() == DAP_JAVA_RESOLVING);
	CHECK(dap_java_step_text() != NULL);
	dap_java_cancel();
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	CHECK(dap_java_step_text() == NULL);
	/* And cancelling nothing is not an error. */
	dap_java_cancel();
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
}

/* Two sequences cannot overlap: the second is refused with a sentence
 * rather than replacing the first, which would leak the first's copy of the
 * launch arguments. */
static void test_a_second_sequence_is_refused(void)
{
	char error[256] = "";

	CHECK(begin("java", "/nonexistent/Fixture.java", error, sizeof(error))
	    == 0);
	CHECK(begin("java", "/nonexistent/Other.java", error, sizeof(error))
	    != 0);
	CHECK(error[0] != '\0');
	dap_java_cancel();
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
}

/* ------------------------ the teardown inference ---------------------- */

/* The inference's shape, driven through the session's event hook exactly as
 * an adapter would drive it, with the thread list standing in for what a
 * `threads` reply produced.
 *
 * These cases do NOT drive a JVM.  What they assert is kg's policy about
 * two facts nbcode has -- no `terminated`, ever, and a thread list that
 * never empties -- and a fixture that produced those facts would be
 * measuring the JVM rather than the policy.  The measurements themselves
 * are in doc/plans/dap/03-java.md and in src/dap_java.c's own comment. */
static void thread_event(const char *reason, int id)
{
	char text[128];
	struct kg_json *doc;

	snprintf(text, sizeof(text), "{\"reason\":\"%s\",\"threadId\":%d}",
	    reason, id);
	doc = kg_json_parse(text, strlen(text), NULL);
	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	dap_java_event(NULL, "thread", kg_json_root(doc));
	kg_json_free(doc);
}

static void stopped_event(void)
{
	static const char text[] = "{\"reason\":\"pause\",\"threadId\":1}";
	struct kg_json *doc = kg_json_parse(text, sizeof(text) - 1, NULL);

	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	dap_java_event(NULL, "stopped", kg_json_root(doc));
	kg_json_free(doc);
}

/* With nothing running, an event is a no-op rather than a crash: the hook
 * is installed for the whole life of a session, including the parts where
 * this module has already let go. */
static void test_events_outside_a_session_are_ignored(void)
{
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	thread_event("exited", 1);
	stopped_event();
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	CHECK(dap_java_poll() == 0);
}

/* ------------------ the teardown inference, both ways ----------------- */

/* The grace these cases wait out.  Short enough that four of them cost a
 * fraction of a second, long enough that the poll following an event is
 * inside the window on any box this suite runs on -- three orders of
 * magnitude of margin, since that poll does no I/O. */
#define GRACE_MS 200u

/* The five threads nbcode was MEASURED to answer `threads` with from the
 * moment the program's own thread exits, and forever afterwards, plus the
 * two kg lists for the same reason src/dap_java.c gives.  All seven are
 * ordinary-looking names -- `Finalizer` is not spelled differently from a
 * thread a program might start -- which is the point: what keeps a thread
 * from counting as proof of life is membership of jvm_system_threads[],
 * and nothing else about it. */
#define JVM_THREADS                                                            \
	"{\"id\":11,\"name\":\"Signal Dispatcher\"},"                          \
	"{\"id\":12,\"name\":\"Reference Handler\"},"                          \
	"{\"id\":13,\"name\":\"Finalizer\"},"                                  \
	"{\"id\":14,\"name\":\"Notification Thread\"},"                        \
	"{\"id\":15,\"name\":\"Common-Cleaner\"},"                             \
	"{\"id\":16,\"name\":\"Attach Listener\"},"                            \
	"{\"id\":17,\"name\":\"process reaper\"}"
#define JVM_THREAD_COUNT 7

/* The two answers a case scripts `threads` with: the program running, and
 * the program gone.  The fake pops them as they are asked for and the last
 * is sticky, so every re-request after the second gets the drained list --
 * which is what a real nbcode does, having no further answer to give. */
#define THREADS_RUNNING                                                        \
	" --body "                                                             \
	"'threads:{\"threads\":[{\"id\":1,\"name\":\"main\"}," JVM_THREADS     \
	"]}'"
#define THREADS_DRAINED " --body 'threads:{\"threads\":[" JVM_THREADS "]}'"

static void nap_ms(unsigned ms)
{
	struct timespec nap = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&nap, NULL);
}

/* Service the session and the model, but NOT the inference: a case that
 * wants time to pass without dap_java_poll() seeing it uses this. */
static void pump_session(struct dap_session *s, double seconds)
{
	double deadline = monotonic_seconds() + seconds;

	while (monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		(void)dap_exec_poll();
		nap_a_millisecond();
	}
}

/* The editor's whole debugger poll, inference included. */
static void pump_everything(struct dap_session *s, double seconds)
{
	double deadline = monotonic_seconds() + seconds;

	while (monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		(void)dap_exec_poll();
		(void)dap_java_poll();
		nap_a_millisecond();
	}
}

/* Wait for the model's thread list to be the one the next scripted answer
 * describes.  A `thread` event opens dap_exec's debounce window and
 * dap_exec_poll() is what eventually asks, so this is the bounded wait for
 * that round trip rather than a sleep sized to the debounce. */
static bool pump_until_threads(struct dap_session *s, size_t want)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;

	while (monotonic_seconds() < deadline) {
		(void)dap_session_poll(s);
		(void)dap_exec_poll();
		if (dap_exec_thread_count() == want) {
			return true;
		}
		nap_a_millisecond();
	}
	return false;
}

/* A RUNNING Java session on the fake, reached the way a real one is: the
 * start sequence resolves an endpoint, connects to it with the secret in
 * front of the first frame, and hands the result to dap_session. */
static struct dap_session *start_java_session(FILE **child, const char *extra)
{
	char error[256] = "";
	unsigned short port = listen_fake(child, SIBLING_SECRET, extra);

	CHECK(port != 0);
	if (!port) {
		return NULL;
	}
	dap_java_set_grace_ms(GRACE_MS);
	dap_java_set_endpoint_for_test("127.0.0.1", port, SIBLING_SECRET);
	CHECK(begin("java", "/nonexistent/Fixture.java", error, sizeof(error))
	    == 0);
	/* One poll is the whole resolve step when the endpoint is already in
	 * hand, which is also what a warm language server gives. */
	(void)dap_java_poll();
	CHECKF(
	    dap_java_step() == DAP_JAVA_RUNNING, "the session never started");
	CHECK(dap_session_current() != NULL);
	return dap_session_current();
}

/* The session is asked for rather than passed in: a case whose session
 * ended under it -- which is the failure every one of these cases is
 * written to catch -- may have had it closed and freed by dap_exec_poll()'s
 * own reaper already, and closing the pointer a second time would turn a
 * reported failure into a crash. */
static void finish_java(FILE *child)
{
	struct dap_session *s = dap_session_current();

	if (s) {
		dap_session_close(s);
	}
	dap_java_session_ended();
	dap_exec_session_ended();
	dap_java_set_endpoint_for_test(NULL, 0, NULL);
	dap_java_set_grace_ms(DAP_JAVA_TEARDOWN_GRACE_MS);
	if (child) {
		pclose(child);
	}
	CHECK(dap_session_current() == NULL);
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
}

/* THE EXPIRY WAY.  A thread exits, the list drains to the JVM's own, and
 * after the grace the session is over -- because nbcode will never say
 * `terminated` about it (measured to 60 s past program completion).
 *
 * Two halves of the policy are asserted here beside the ending itself.  The
 * list is not EMPTY when it fires: seven threads are still in it, which is
 * exactly the shape that would leave a "wait for no threads" client hanging
 * until the user killed it.  And the ending is a DISCONNECT: the adapter
 * lives inside the user's language server, so the session closes one socket
 * and terminating anything would take the server with it. */
static void test_the_grace_ends_a_session_nbcode_never_terminates(void)
{
	FILE *child = NULL;
	struct dap_session *s
	    = start_java_session(&child, THREADS_RUNNING THREADS_DRAINED);
	struct dap_session_state st;

	if (!s) {
		if (child) {
			pclose(child);
		}
		return;
	}
	/* The program is running: its own thread among the JVM's. */
	thread_event("started", 1);
	CHECK(pump_until_threads(s, JVM_THREAD_COUNT + 1));
	/* And it ends. */
	thread_event("exited", 1);
	CHECKF(pump_until_threads(s, JVM_THREAD_COUNT),
	    "the thread list never drained");
	CHECK(dap_java_step() == DAP_JAVA_RUNNING);
	/* The first poll after the drain ARMS the window; it does not end
	 * anything, which is the whole difference between a grace and a
	 * trigger. */
	CHECK(dap_java_poll() == 0);
	CHECK(dap_java_step() == DAP_JAVA_RUNNING);
	nap_ms(GRACE_MS + 100u);
	CHECK(dap_java_poll() == 1);
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	CHECKF(strstr(heard.last, "finished") != NULL,
	    "the ending was reported as: %s", heard.last);
	CHECK(heard.errors == 0);
	dap_session_get_state(s, &st);
	CHECKF(st.end_request == DAP_END_DISCONNECT,
	    "the session ended as request %d", (int)st.end_request);
	CHECK(st.phase == DAP_PHASE_ENDING || st.phase == DAP_PHASE_DEAD);
	finish_java(child);
}

/* THE CANCEL WAY, the durable one.  A thread of the program has exited --
 * so the inference is armed as far as `exited_seen` goes -- but another
 * thread of the program is still in the list, and that is proof of life for
 * as long as it lasts.  Polled all the way through several graces, nothing
 * is inferred.
 *
 * This is the case that matters most: a client that read one
 * `thread{exited}` as "the session is over" would end every session at its
 * first worker thread. */
static void test_a_live_debuggee_thread_never_lets_the_grace_expire(void)
{
	FILE *child = NULL;
	struct dap_session *s = start_java_session(&child, THREADS_RUNNING);

	if (!s) {
		if (child) {
			pclose(child);
		}
		return;
	}
	thread_event("started", 1);
	CHECK(pump_until_threads(s, JVM_THREAD_COUNT + 1));
	/* A worker ends.  `main` is still in the list. */
	thread_event("exited", 99);
	pump_everything(s, (double)(GRACE_MS * 4u) / 1000.0);
	CHECKF(dap_java_step() == DAP_JAVA_RUNNING,
	    "the session was ended with a live thread in the list");
	CHECK(dap_session_current() == s);
	CHECKF(strstr(heard.last, "finished") == NULL,
	    "reported the program finished: %s", heard.last);
	finish_java(child);
}

/* THE CANCEL WAY, the other one: with the list already drained and the
 * window already running, an EVENT that proves the session is alive resets
 * it.  Both events that do so are here -- a `stopped`, which is how a
 * session with breakpoints spends most of its time, and a thread STARTED,
 * which is a program that was merely between threads.
 *
 * Each of them is asserted the same way: let more than a full window pass
 * with nothing polling the inference, then send the event and poll.  That
 * poll would have ended the session had the event not moved the window on,
 * which is what makes it an assertion about the cancel rather than about
 * the clock.  The last window is left undisturbed, so the case also says
 * the cancels RESET the inference rather than switching it off. */
static void test_proof_of_life_resets_a_running_grace(void)
{
	FILE *child = NULL;
	struct dap_session *s
	    = start_java_session(&child, THREADS_RUNNING THREADS_DRAINED);

	if (!s) {
		if (child) {
			pclose(child);
		}
		return;
	}
	thread_event("started", 1);
	CHECK(pump_until_threads(s, JVM_THREAD_COUNT + 1));
	thread_event("exited", 1);
	CHECKF(pump_until_threads(s, JVM_THREAD_COUNT),
	    "the thread list never drained");
	CHECK(dap_java_poll() == 0);
	pump_session(s, (double)(GRACE_MS + 100u) / 1000.0);
	stopped_event();
	CHECK(dap_java_poll() == 0);
	CHECKF(dap_java_step() == DAP_JAVA_RUNNING,
	    "a stop did not cancel the inference");
	pump_session(s, (double)(GRACE_MS + 100u) / 1000.0);
	thread_event("started", 2);
	CHECK(dap_java_poll() == 0);
	CHECKF(dap_java_step() == DAP_JAVA_RUNNING,
	    "a started thread did not cancel the inference");
	/* Nothing proves anything for one more window, and the inference
	 * fires after all: a cancel is a reset. */
	nap_ms(GRACE_MS + 100u);
	CHECK(dap_java_poll() == 1);
	CHECK(dap_java_step() == DAP_JAVA_IDLE);
	CHECKF(strstr(heard.last, "finished") != NULL,
	    "the ending was reported as: %s", heard.last);
	finish_java(child);
}

/* And the guard the whole policy rests on: with no thread having exited,
 * there is nothing to infer however few threads the list has.  A session
 * whose program has not started its own thread yet -- the JVM's seven and
 * nothing else, which is what `threads` answers before a launch gets going
 * -- is left alone. */
static void test_without_an_exited_thread_nothing_is_inferred(void)
{
	FILE *child = NULL;
	struct dap_session *s = start_java_session(&child, THREADS_DRAINED);

	if (!s) {
		if (child) {
			pclose(child);
		}
		return;
	}
	thread_event("started", 11);
	CHECK(pump_until_threads(s, JVM_THREAD_COUNT));
	pump_everything(s, (double)(GRACE_MS * 4u) / 1000.0);
	CHECKF(dap_java_step() == DAP_JAVA_RUNNING,
	    "a session with no exited thread was ended anyway");
	CHECK(dap_session_current() == s);
	finish_java(child);
}

/* ------------------------------- the config --------------------------- */

/* The built-in Java configuration, and the shape it carries.  `${fileUri}`
 * rather than `${file}` because nbcode parses that argument as a URI and
 * not as a path (debugger.ts:199-224); `classPaths:["any"]` because that is
 * what asks the single-file launcher to work it out. */
static void test_the_builtin_java_adapter_is_a_sibling(void)
{
	const struct dap_adapter_spec *spec = dap_config_builtin("nbcode-java");

	CHECK(spec != NULL);
	if (!spec) {
		return;
	}
	CHECK(spec->transport == DAP_TRANSPORT_LSP_SIBLING);
	CHECK(strcmp(spec->lsp_language, "java") == 0);
	/* No command of its own, and no environment override for one: there
	 * is nothing to replace, and the way to point kg at another nbcode
	 * is KG_LSP_SERVER_JAVA. */
	CHECK(spec->argv_count == 0);
	CHECK(spec->command[0] == '\0');
	CHECK(spec->env_override[0] == '\0');
	CHECK(dap_java_owns(spec));
}

static void test_the_other_builtin_adapters_are_not_siblings(void)
{
	const struct dap_adapter_spec *spec;

	spec = dap_config_builtin("lldb-dap");
	CHECK(spec && spec->transport == DAP_TRANSPORT_STDIO);
	CHECK(spec && spec->lsp_language[0] == '\0');
	spec = dap_config_builtin("debugpy");
	CHECK(spec && spec->transport == DAP_TRANSPORT_STDIO);
	CHECK(spec && spec->lsp_language[0] == '\0');
}

/* ${fileUri}: a URI, percent-encoded, and refused rather than guessed at
 * for a path that is not absolute.  The encoding rule is RFC 3986's
 * unreserved set plus `/`, written here a second time on purpose -- the
 * debugger must not link src/lsp_uri.c, which does not exist in a
 * WITH_LSP=0 build. */
static void test_file_uri_encodes_the_path(void)
{
	struct dap_config_context ctx = { 0 };
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char out[512];

	ctx.file = "/tmp/proj/Fixture.java";
	CHECK(
	    dap_config_expand_string("${fileUri}", &ctx, out, sizeof(out), &err)
	    == 0);
	CHECK(strcmp(out, "file:///tmp/proj/Fixture.java") == 0);

	ctx.file = "/tmp/a b/Fix+ture.java";
	CHECK(
	    dap_config_expand_string("${fileUri}", &ctx, out, sizeof(out), &err)
	    == 0);
	CHECKF(strcmp(out, "file:///tmp/a%20b/Fix%2Bture.java") == 0,
	    "encoded to %s", out);

	/* Two bytes of one UTF-8 character, each percent-encoded: the rule
	 * is per BYTE, which is what a server decoding it expects. */
	ctx.file = "/tmp/\xc3\xa5.java";
	CHECK(
	    dap_config_expand_string("${fileUri}", &ctx, out, sizeof(out), &err)
	    == 0);
	CHECKF(
	    strcmp(out, "file:///tmp/%C3%A5.java") == 0, "encoded to %s", out);
}

static void test_file_uri_refuses_what_it_cannot_encode(void)
{
	struct dap_config_context ctx = { 0 };
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char out[512];

	/* A buffer visiting no file. */
	ctx.file = NULL;
	CHECK(
	    dap_config_expand_string("${fileUri}", &ctx, out, sizeof(out), &err)
	    != 0);
	CHECK(err.message[0] != '\0');

	/* And an empty path, which is the same thing spelled differently. */
	ctx.file = "";
	err.message[0] = '\0';
	CHECK(
	    dap_config_expand_string("${fileUri}", &ctx, out, sizeof(out), &err)
	    != 0);
	CHECK(err.message[0] != '\0');
}

/* A RELATIVE path is not refused, it is made absolute -- against kg's own
 * working directory, which is what such a path means.  `kg Fixture.java`
 * visits a real saved file, and refusing to debug it because the name kg
 * was handed had no leading slash would be a debugger that cannot debug
 * the file it was started on. */
static void test_file_uri_makes_a_relative_path_absolute(void)
{
	struct dap_config_context ctx = { 0 };
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char cwd[PATH_MAX];
	char want[PATH_MAX + 64];
	char out[512];

	if (!getcwd(cwd, sizeof(cwd))) {
		CHECKF(false, "no working directory");
		return;
	}
	ctx.file = "Fixture.java";
	CHECK(
	    dap_config_expand_string("${fileUri}", &ctx, out, sizeof(out), &err)
	    == 0);
	snprintf(want, sizeof(want), "file://%s/Fixture.java", cwd);
	CHECKF(strcmp(out, want) == 0, "expanded to %s, wanted %s", out, want);
}

/* The substitution set stays CLOSED: a near miss is an error and not an
 * empty string, which is the rule the whole set exists to keep. */
static void test_a_misspelled_file_uri_is_still_an_error(void)
{
	struct dap_config_context ctx = { 0 };
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	char out[512];

	ctx.file = "/tmp/Fixture.java";
	CHECK(
	    dap_config_expand_string("${fileURI}", &ctx, out, sizeof(out), &err)
	    != 0);
	CHECK(
	    dap_config_expand_string("${fileUr}", &ctx, out, sizeof(out), &err)
	    != 0);
	/* And ${file} still means the path, unencoded. */
	CHECK(dap_config_expand_string("${file}", &ctx, out, sizeof(out), &err)
	    == 0);
	CHECK(strcmp(out, "/tmp/Fixture.java") == 0);
}

/* `lsp-sibling` is a transport a configuration file may now name, and one
 * that names it needs a language rather than a command. */
static void test_a_file_can_name_the_sibling_transport(void)
{
	static const char text[]
	    = "{\"version\":1,\"configurations\":[{\"name\":\"j\","
	      "\"request\":\"launch\",\"adapter\":{\"transport\":"
	      "\"lsp-sibling\",\"lspLanguage\":\"java\"},"
	      "\"arguments\":{}}]}";
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	struct dap_config_set *set = dap_config_parse(
	    text, sizeof(text) - 1, "/tmp/.kg-dap.json", "/tmp", &err);

	CHECKF(set != NULL, "%s", err.message);
	if (!set) {
		return;
	}
	CHECK(set->count == 1);
	CHECK(set->configs[0].inline_adapter != NULL);
	if (set->configs[0].inline_adapter) {
		CHECK(set->configs[0].inline_adapter->transport
		    == DAP_TRANSPORT_LSP_SIBLING);
		CHECK(
		    strcmp(set->configs[0].inline_adapter->lsp_language, "java")
		    == 0);
	}
	dap_config_free(set);
}

static void test_a_sibling_without_a_language_is_refused(void)
{
	static const char text[]
	    = "{\"version\":1,\"configurations\":[{\"name\":\"j\","
	      "\"request\":\"launch\",\"adapter\":{\"transport\":"
	      "\"lsp-sibling\"},\"arguments\":{}}]}";
	struct dap_config_error err = { "", "", DAP_CONFIG_NO_OFFSET };
	struct dap_config_set *set = dap_config_parse(
	    text, sizeof(text) - 1, "/tmp/.kg-dap.json", "/tmp", &err);

	CHECK(set == NULL);
	CHECK(err.message[0] != '\0');
	dap_config_free(set);
}

int main(void)
{
	const char *dir = getenv("KG_TEST_SRCDIR");

	snprintf(script_path, sizeof(script_path), "%s/fake_dap_adapter.py",
	    dir ? dir : "test");

	RUN(test_sibling_connect_writes_the_secret_first);
	RUN(test_sibling_connect_with_the_wrong_secret_dies);
	RUN(test_attach_socket_makes_the_descriptor_nonblocking);

	RUN(test_an_unknown_language_has_no_sibling);
	RUN(test_every_sibling_status_has_a_sentence);

	RUN(test_only_the_sibling_kind_is_javas);
	RUN(test_an_unsaved_buffer_is_refused_up_front);
	RUN(test_a_language_with_no_sibling_says_which_server_it_wants);
	RUN(test_a_waiting_sequence_can_be_cancelled);
	RUN(test_a_second_sequence_is_refused);
	RUN(test_events_outside_a_session_are_ignored);
	RUN(test_the_grace_ends_a_session_nbcode_never_terminates);
	RUN(test_a_live_debuggee_thread_never_lets_the_grace_expire);
	RUN(test_proof_of_life_resets_a_running_grace);
	RUN(test_without_an_exited_thread_nothing_is_inferred);

	RUN(test_the_builtin_java_adapter_is_a_sibling);
	RUN(test_the_other_builtin_adapters_are_not_siblings);
	RUN(test_file_uri_encodes_the_path);
	RUN(test_file_uri_refuses_what_it_cannot_encode);
	RUN(test_file_uri_makes_a_relative_path_absolute);
	RUN(test_a_misspelled_file_uri_is_still_an_error);
	RUN(test_a_file_can_name_the_sibling_transport);
	RUN(test_a_sibling_without_a_language_is_refused);
	return test_summary();
}
