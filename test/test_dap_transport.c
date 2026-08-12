#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_dap_transport.c — the debug adapter's wire (src/dap_transport.c,
 * stage 2 of doc/plans/dap/01-protocol.md).
 *
 * There is no fake pipe here and no injected byte stream: every case runs a
 * real process and talks to it over real descriptors, because what this
 * module can get wrong -- a child killed that was never ours, a child left
 * behind that was, a write side closed under the message that ends the
 * session -- is wrong about processes and descriptors and about nothing
 * smaller.  Most children are test/fake_dap_adapter.py, whose modes are the
 * adapter behaviours measured against lldb-dap and debugpy.
 *
 * The framing cases are thin on purpose.  Framing is framed_io's, proven by
 * test_framed_io.c and test_lsp_transport.c; what they assert here is that
 * this module composes it rather than reimplements it.  The weight is on
 * the two things that are new: who owns the child, and the end-of-session
 * ladder.
 *
 * Nothing sleeps blind.  Every wait is a bounded pump that drives what a
 * poll site would drive, so a case that would hang fails in a few seconds
 * naming the assertion that hung.
 */

#include "../src/dap_transport.h"
#include "../src/process.h"
#include "test.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a loaded box or a valgrind lane never trips it, short
 * enough that a genuine hang is reported rather than waited out. */
#define PUMP_DEADLINE_SECONDS 10.0

/* Bigger than any pipe buffer, so a send of it is guaranteed to leave bytes
 * queued and the half-close ordering is a real question rather than a race
 * the case usually wins. */
#define BIG_BODY_BYTES (256u * 1024u)

static char script_path[1024];

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

/* Drive both directions until a message is delivered, the transport dies,
 * or the deadline passes.  Returns what dap_transport_next_message()
 * returned last, or 0 on timeout -- which a caller asserting 1 or -1
 * reports as the failure it is. */
static int pump_until_message(
    struct dap_transport *t, const char **body, size_t *len)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	int rc;

	for (;;) {
		if (dap_transport_pending_bytes(t) > 0) {
			(void)dap_transport_flush(t);
		}
		rc = dap_transport_next_message(t, body, len);
		if (rc != 0) {
			return rc;
		}
		if (monotonic_seconds() >= deadline) {
			return 0;
		}
		nap_a_millisecond();
	}
}

static bool line_contains(const char *line, size_t len, const char *needle)
{
	size_t needle_len = strlen(needle);
	size_t i;

	if (needle_len > len) {
		return false;
	}
	for (i = 0; i + needle_len <= len; i++) {
		if (memcmp(line + i, needle, needle_len) == 0) {
			return true;
		}
	}
	return false;
}

/* Drive the ladder and the adapter's log channel until the adapter has said
 * `needle`.  Both at once because that is what a poll site does, and
 * because the verdict a lifecycle case wants is often a line the adapter
 * writes while the ladder is still running. */
static bool pump_until_log_line(struct dap_transport *t, const char *needle)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	const char *line = NULL;
	size_t len = 0;

	for (;;) {
		(void)dap_transport_shutdown_poll(t);
		while (dap_transport_next_log_line(t, &line, &len) == 1) {
			if (line_contains(line, len, needle)) {
				return true;
			}
		}
		if (monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
}

static unsigned shutdown_bit(enum dap_transport_shutdown_state state)
{
	return 1u << (unsigned)state;
}

/* Drive the ladder to DONE, returning the set of rungs it stood on -- so a
 * case can say not only where it ended but which escalations it took to get
 * there, which is the whole difference between the grace step working and
 * the kill backstop covering for it. */
static unsigned pump_shutdown(struct dap_transport *t)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	unsigned seen = shutdown_bit(dap_transport_shutdown_state(t));

	while (dap_transport_shutdown_poll(t) != 0) {
		seen |= shutdown_bit(dap_transport_shutdown_state(t));
		if (monotonic_seconds() >= deadline) {
			return seen;
		}
		nap_a_millisecond();
	}
	return seen | shutdown_bit(dap_transport_shutdown_state(t));
}

/* Drive the outbox empty, so a case that is about what happens AFTER a send
 * is not really about whether the send had landed. */
static bool pump_until_drained(struct dap_transport *t)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;

	while (dap_transport_pending_bytes(t) > 0) {
		if (dap_transport_flush(t) < 0
		    || monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
	return true;
}

/* Deadlines that have already expired, so the ladder escalates on the poll
 * after the one that entered a rung.  This is how a case drives the whole
 * ladder without waiting a real grace period out. */
static void set_zero_deadlines(struct dap_transport *t)
{
	struct dap_transport_deadlines limits = { 0, 0, 0 };

	dap_transport_set_deadlines(t, &limits);
}

/* Whether this pid was collected by somebody: waitpid() answering ECHILD is
 * the only proof from outside that a child is not a zombie somebody forgot. */
static bool child_was_reaped(pid_t pid)
{
	int status;

	errno = 0;
	return waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD;
}

static void fake_argv(const char **argv, size_t max, const char *mode,
    const char *opt, const char *value, bool listen)
{
	size_t n = 0;

	argv[n++] = "python3";
	argv[n++] = script_path;
	argv[n++] = "--mode";
	argv[n++] = mode;
	if (listen) {
		argv[n++] = "--listen";
	}
	if (opt) {
		argv[n++] = opt;
		/* A flag with no value of its own is spelled with a NULL
		 * value rather than by a second helper. */
		if (value) {
			argv[n++] = value;
		}
	}
	argv[n] = NULL;
	CHECK(n < max);
}

static struct dap_transport *start_fake(
    const char *mode, const char *opt, const char *value)
{
	const char *argv[9];
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = -1,
	};

	fake_argv(
	    argv, sizeof(argv) / sizeof(argv[0]), mode, opt, value, false);
	return dap_transport_start_stdio(&req);
}

/* Read one newline-terminated line from a descriptor, bounded by the pump
 * deadline.  Used only on the fixture's own pipe -- the transport never
 * reads like this. */
static bool read_line_from(int fd, char *out, size_t cap)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	size_t used = 0;
	ssize_t n;

	while (used + 1 < cap) {
		n = read(fd, out + used, 1);
		if (n == 1) {
			if (out[used] == '\n') {
				out[used] = '\0';
				return true;
			}
			used++;
			continue;
		}
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)
		    || monotonic_seconds() >= deadline) {
			return false;
		}
		nap_a_millisecond();
	}
	return false;
}

/* An adapter this test owns: it listens, announces its port on its own
 * output, and is nobody's child but this fixture's.  Attaching to it is the
 * tcp row of the ownership table, and the point of the fixture is that the
 * transport must leave this process entirely alone. */
static bool start_listening_fake(
    const char *mode, pid_t *pid, int *out_fd, unsigned short *port)
{
	const char *argv[9];
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = -1,
		/* One pipe for both, so the fixture reads the port announce
		 * and the adapter's later verdict from one descriptor.  It is
		 * the fixture's own arrangement, not a transport's: the
		 * transport never joins the two. */
		.stderr_to_output = true,
		.nonblocking_output = true,
	};
	char line[128];
	int value = 0;

	fake_argv(argv, sizeof(argv) / sizeof(argv[0]), mode, NULL, NULL, true);
	if (kg_process_spawn(&req, pid, out_fd) != 0) {
		return false;
	}
	if (!read_line_from(*out_fd, line, sizeof(line))
	    || sscanf(line, "PORT %d", &value) != 1 || value <= 0
	    || value > 65535) {
		return false;
	}
	*port = (unsigned short)value;
	return true;
}

static void stop_listening_fake(pid_t pid, int out_fd)
{
	struct kg_process_status status;

	kg_process_signal_group(pid, SIGKILL);
	kg_process_wait(pid, &status);
	kg_close_fd(&out_fd);
}

/* --- framing: that the composition is a composition ------------------- */

static void test_stdio_echo_round_trip(void)
{
	struct dap_transport *t = start_fake("echo", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_kind(t) == DAP_TRANSPORT_STDIO);
	CHECK(dap_transport_send(t, "{\"seq\":1}", 9) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 9 && body && memcmp(body, "{\"seq\":1}", 9) == 0);
	CHECK(!dap_transport_failed(t));
	CHECK(dap_transport_pending_bytes(t) == 0);
	dap_transport_close(t);
}

static void test_split_delivery_is_reassembled(void)
{
	struct dap_transport *t = start_fake("split", "--chunk", "1");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_send(t, "abcdefghij", 10) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 10 && body && memcmp(body, "abcdefghij", 10) == 0);
	dap_transport_close(t);
}

static void test_batched_frames_come_out_of_one_read(void)
{
	struct dap_transport *t = start_fake("batch", "--count", "3");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_send(t, "one", 3) == 0);
	CHECK(dap_transport_send(t, "twotwo", 6) == 0);
	CHECK(dap_transport_send(t, "three33", 7) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 3 && body && memcmp(body, "one", 3) == 0);
	/* No pumping: the other two must already be buffered. */
	CHECK(dap_transport_next_message(t, &body, &len) == 1);
	CHECK(len == 6 && body && memcmp(body, "twotwo", 6) == 0);
	CHECK(dap_transport_next_message(t, &body, &len) == 1);
	CHECK(len == 7 && body && memcmp(body, "three33", 7) == 0);
	dap_transport_close(t);
}

static void test_garbage_is_a_protocol_error(void)
{
	struct dap_transport *t = start_fake("garbage", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(dap_transport_failed(t));
	CHECK(dap_transport_error(t) == FRAMED_IO_ERR_PROTOCOL);
	CHECK(!dap_transport_error_truncated(t));
	dap_transport_close(t);
}

static void test_huge_content_length_is_refused(void)
{
	struct dap_transport *t = start_fake("huge-header", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(dap_transport_error(t) == FRAMED_IO_ERR_TOO_LARGE);
	/* The adapter sent it; kg did not queue it. */
	CHECK(!dap_transport_error_outbound(t));
	dap_transport_close(t);
}

static void test_truncated_frame_is_reported_as_truncation(void)
{
	struct dap_transport *t = start_fake("truncated", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(dap_transport_error(t) == FRAMED_IO_ERR_PROTOCOL);
	CHECK(dap_transport_error_truncated(t));
	dap_transport_close(t);
}

/* --- ownership -------------------------------------------------------- */

/* The table in dap_transport.h answers for all four kinds, including the
 * two with no constructor yet: a close path asks this before it signals,
 * and the answer must not depend on which stage shipped the kind. */
static void test_every_kind_answers_the_ownership_table(void)
{
	CHECK(dap_transport_kind_owns_child(DAP_TRANSPORT_STDIO));
	CHECK(!dap_transport_kind_owns_child(DAP_TRANSPORT_TCP_ATTACH));
	CHECK(dap_transport_kind_owns_child(DAP_TRANSPORT_SPAWN_PORT));
	CHECK(!dap_transport_kind_owns_child(DAP_TRANSPORT_LSP_SIBLING));
	CHECK(!dap_transport_kind_owns_child(
	    (enum dap_transport_kind)(DAP_TRANSPORT_LSP_SIBLING + 1)));
}

/* A spawned adapter is handed NO controlling terminal.  The difference only
 * shows up through a program the adapter starts: debugpy's launcher hands
 * the terminal's foreground process group to the debuggee, and a debuggee
 * that inherited kg's terminal takes kg's own keyboard with it -- kg reads
 * EIO the moment the program finishes and exits, at the end of every
 * completed debug run (measured).  The adapter answers for it, since
 * whether `/dev/tty` opens is a fact about the child and not about the
 * parent's intentions. */
static void test_a_spawned_adapter_gets_no_controlling_terminal(void)
{
	struct dap_transport *t = start_fake("linger", "--report-tty", NULL);

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	/* And still leads its own process group, which is what every signal
	 * path in this module takes. */
	CHECKF(getpgid(dap_transport_pid(t)) == dap_transport_pid(t),
	    "the adapter leads no process group of its own");
	CHECKF(pump_until_log_line(t, "tty: no"),
	    "the adapter was handed kg's controlling terminal");
	dap_transport_close(t);
}

/* process.h:68-78: this function makes the child's stdin itself, so a
 * descriptor in the request is a caller bug.  The field defaults to 0 --
 * kg's own terminal -- in a designated initializer that forgets it, which
 * is exactly the mistake worth failing on rather than inheriting. */
static void test_stdio_refuses_a_caller_supplied_stdin(void)
{
	const char *argv[] = { "/bin/cat", NULL };
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = 0,
	};

	errno = 0;
	CHECK(dap_transport_start_stdio(&req) == NULL);
	CHECK(errno == EINVAL);
}

/* The adapter's standard error is a channel of its own.  A log line spliced
 * into the frame stream desynchronises the framing for good, so this case
 * asks for both at once: the line comes out of the log channel, and the
 * round trip on the frame stream is untouched by it. */
static void test_stderr_is_a_channel_of_its_own(void)
{
	struct dap_transport *t
	    = start_fake("echo", "--stderr", "adapter noise");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_send(t, "ping", 4) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 4 && body && memcmp(body, "ping", 4) == 0);
	CHECK(pump_until_log_line(t, "adapter noise"));
	dap_transport_close(t);
}

/* User cancel, which is the failure path with the least cooperation in it:
 * no ladder ran, the adapter ignores SIGTERM, and close() must still come
 * back with the child collected rather than left for init. */
static void test_close_reaps_a_child_that_ignores_term(void)
{
	struct dap_transport *t = start_fake("ignore-term", NULL, NULL);
	pid_t pid;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	pid = dap_transport_pid(t);
	CHECK(pid > 0);
	CHECK(pump_until_log_line(t, "signals ignored"));
	dap_transport_close(t);
	CHECK(child_was_reaped(pid));
}

/* The tcp row: no child is ours, so no close path may touch one.  The
 * adapter here is the fixture's own process, and the proof that the
 * transport left it alone is that it goes on talking after the transport is
 * gone -- and that the pid the transport would have signalled is -1 rather
 * than the 0 that means kg's own process group. */
static void test_tcp_attach_close_never_signals_the_adapter(void)
{
	struct dap_transport *t;
	const char *body = NULL;
	unsigned short port = 0;
	size_t len = 0;
	pid_t pid = -1;
	int out_fd = -1;
	char line[256];

	CHECK(start_listening_fake("linger", &pid, &out_fd, &port));
	if (port == 0) {
		return;
	}
	t = dap_transport_attach_tcp("127.0.0.1", port);
	CHECK(t != NULL);
	if (!t) {
		stop_listening_fake(pid, out_fd);
		return;
	}
	CHECK(dap_transport_kind(t) == DAP_TRANSPORT_TCP_ATTACH);
	CHECK(dap_transport_pid(t) == -1);
	CHECK(!dap_transport_child_alive(t));
	CHECK(dap_transport_send(t, "{\"seq\":7}", 9) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 9 && body && memcmp(body, "{\"seq\":7}", 9) == 0);
	dap_transport_close(t);
	/* Alive after the close, and saying so on the fixture's own pipe. */
	CHECK(read_line_from(out_fd, line, sizeof(line)));
	CHECK(line_contains(line, strlen(line), "linger: input ended"));
	CHECK(!child_was_reaped(pid));
	stop_listening_fake(pid, out_fd);
}

/* Having no child is not having a shutdown to skip: the ladder still runs,
 * it just has nothing to wait for once the socket is half-closed, so it
 * reaches DONE on the first poll and never stands on a signalling rung. */
static void test_tcp_attach_shutdown_is_a_half_close_and_nothing_else(void)
{
	struct dap_transport *t;
	unsigned short port = 0;
	unsigned seen;
	pid_t pid = -1;
	int out_fd = -1;

	CHECK(start_listening_fake("linger", &pid, &out_fd, &port));
	if (port == 0) {
		return;
	}
	t = dap_transport_attach_tcp("127.0.0.1", port);
	CHECK(t != NULL);
	if (!t) {
		stop_listening_fake(pid, out_fd);
		return;
	}
	CHECK(dap_transport_send(t, "bye", 3) == 0);
	CHECK(pump_until_drained(t));
	/* One rung per poll: the drained outbox half-closes the socket and
	 * the grace begins, with nothing alive for it to wait for. */
	CHECK(dap_transport_begin_shutdown(t) == 1);
	CHECK(dap_transport_shutdown_state(t) == DAP_TRANSPORT_SHUTDOWN_GRACE);
	seen = pump_shutdown(t);
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_DONE));
	CHECK((seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_SIGNALLED)) == 0);
	CHECK((seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_KILLED)) == 0);
	/* Idempotent: a second request changes nothing. */
	CHECK(dap_transport_begin_shutdown(t) == 0);
	CHECK(dap_transport_shutdown_state(t) == DAP_TRANSPORT_SHUTDOWN_DONE);
	dap_transport_close(t);
	CHECK(!child_was_reaped(pid));
	stop_listening_fake(pid, out_fd);
}

/* framed_io accepts only the numeric loopback address in v1.  A rejected
 * argument leaves it healthy and reports through errno, which would make a
 * caller read one failure two ways; the transport records it where every
 * other failure is recorded. */
static void test_attach_to_a_rejected_host_is_a_failed_transport(void)
{
	struct dap_transport *t = dap_transport_attach_tcp("localhost", 4711);
	int fds[DAP_TRANSPORT_WAIT_FDS_MAX];

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_failed(t));
	CHECK(dap_transport_error(t) == FRAMED_IO_ERR_IO);
	CHECK(dap_transport_pid(t) == -1);
	CHECK(dap_transport_wait_fds(t, fds, DAP_TRANSPORT_WAIT_FDS_MAX) == 0);
	dap_transport_close(t);
}

/* --- the end-of-session ladder [M-8] ---------------------------------- */

/* The ordering rule: the write side closes only once the outbox has
 * drained.  The message queued here is far larger than any pipe buffer, so
 * a half-close done at begin_shutdown would cut its body in half -- and the
 * adapter counts what actually arrived, which is how the case can tell the
 * two apart from outside. */
static void test_half_close_waits_for_the_outbox(void)
{
	struct dap_transport *t = start_fake("half-close", NULL, NULL);
	char *big = malloc(BIG_BODY_BYTES);

	CHECK(t != NULL && big != NULL);
	if (!t || !big) {
		free(big);
		dap_transport_close(t);
		return;
	}
	memset(big, 'x', BIG_BODY_BYTES);
	CHECK(dap_transport_send(t, big, BIG_BODY_BYTES) == 0);
	CHECK(dap_transport_pending_bytes(t) > 0);
	CHECK(dap_transport_begin_shutdown(t) == 1);
	CHECK(pump_until_log_line(t, "frames=1 bytes=262144"));
	CHECK(pump_shutdown(t) & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_DONE));
	free(big);
	dap_transport_close(t);
}

/* debugpy, measured: the adapter answers `disconnect` and stays alive.  The
 * session must be able to end on that response, so the transport must reach
 * the half-close with the adapter still there and no end of stream in
 * sight -- and must then get rid of it without ever being told to. */
static void test_session_ends_on_the_response_while_the_adapter_lingers(void)
{
	struct dap_transport *t = start_fake("linger", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;
	unsigned seen;
	pid_t pid;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	pid = dap_transport_pid(t);
	CHECK(dap_transport_send(t, "disconnect", 10) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 10 && body && memcmp(body, "disconnect", 10) == 0);
	CHECK(pump_until_drained(t));
	CHECK(dap_transport_begin_shutdown(t) == 1);
	/* Half-closed, grace running, adapter alive, stream healthy: the
	 * session is over and nothing about the transport says so yet. */
	CHECK(dap_transport_shutdown_state(t) == DAP_TRANSPORT_SHUTDOWN_GRACE);
	CHECK(dap_transport_child_alive(t));
	CHECK(!dap_transport_failed(t));
	set_zero_deadlines(t);
	seen = pump_shutdown(t);
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_DONE));
	CHECK(!dap_transport_child_alive(t));
	dap_transport_close(t);
	CHECK(child_was_reaped(pid));
}

/* The grace rung doing its own job: an adapter that would have lingered for
 * five seconds is asked to leave with SIGTERM, takes it, and the backstop
 * below is never reached. */
static void test_grace_expiry_escalates_to_term(void)
{
	struct dap_transport *t = start_fake("linger", NULL, NULL);
	struct dap_transport_deadlines limits = { 0, 0, 5000 };
	unsigned seen;
	pid_t pid;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	pid = dap_transport_pid(t);
	dap_transport_set_deadlines(t, &limits);
	CHECK(dap_transport_begin_shutdown(t) == 1);
	seen = pump_shutdown(t);
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_SIGNALLED));
	CHECK((seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_KILLED)) == 0);
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_DONE));
	dap_transport_close(t);
	CHECK(child_was_reaped(pid));
}

/* The bottom of the ladder.  This adapter ignores SIGTERM, which is not a
 * hypothetical -- a shell wrapper around a real adapter traps it routinely
 * -- so the kill deadline is what ends the session, and the reap is what
 * keeps a zombie out of the editor's process table. */
static void test_kill_backstop_reaps_an_adapter_that_ignores_term(void)
{
	struct dap_transport *t = start_fake("ignore-term", NULL, NULL);
	unsigned seen;
	pid_t pid;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	pid = dap_transport_pid(t);
	CHECK(pump_until_log_line(t, "signals ignored"));
	set_zero_deadlines(t);
	CHECK(dap_transport_begin_shutdown(t) == 1);
	seen = pump_shutdown(t);
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_SIGNALLED));
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_KILLED));
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_DONE));
	CHECK(!dap_transport_child_alive(t));
	dap_transport_close(t);
	CHECK(child_was_reaped(pid));
}

/* The other half of the EOF rule: an adapter that crashed says nothing at
 * all, and end of stream is then the only edge there is.  The ladder run
 * afterwards has nothing left to do and must not invent work -- no signal
 * is sent to a pid that has already gone. */
static void test_eof_alone_ends_a_crashed_adapter(void)
{
	struct dap_transport *t = start_fake("crash", NULL, NULL);
	int fds[DAP_TRANSPORT_WAIT_FDS_MAX];
	const char *body = NULL;
	size_t len = 0;
	unsigned seen;
	pid_t pid;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	pid = dap_transport_pid(t);
	CHECK(dap_transport_send(t, "initialize", 10) == 0);
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(dap_transport_error(t) == FRAMED_IO_ERR_EOF);
	CHECK(dap_transport_wait_fds(t, fds, DAP_TRANSPORT_WAIT_FDS_MAX) == 0);
	/* The descriptors are already gone, so there is no half-close left to
	 * perform: the ladder starts at the grace it owes a child that has
	 * already taken it. */
	CHECK(dap_transport_begin_shutdown(t) == 1);
	CHECK(dap_transport_shutdown_state(t) == DAP_TRANSPORT_SHUTDOWN_GRACE);
	seen = pump_shutdown(t);
	CHECK(seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_DONE));
	CHECK((seen & shutdown_bit(DAP_TRANSPORT_SHUTDOWN_SIGNALLED)) == 0);
	dap_transport_close(t);
	CHECK(child_was_reaped(pid));
}

/* lldb-dap's shape: the response and the end of stream can arrive in one
 * read.  A transport that reported the death first would lose the very
 * message the session ends on. */
static void test_response_is_delivered_before_the_end_of_stream(void)
{
	struct dap_transport *t = start_fake("respond-then-exit", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_send(t, "disconnect", 10) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 10 && body && memcmp(body, "disconnect", 10) == 0);
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(dap_transport_error(t) == FRAMED_IO_ERR_EOF);
	dap_transport_close(t);
}

/* --- the caller's wait set -------------------------------------------- */

static void test_wait_fds_report_the_stream_and_the_log(void)
{
	struct dap_transport *t = start_fake("echo", NULL, NULL);
	int fds[DAP_TRANSPORT_WAIT_FDS_MAX];

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(dap_transport_wait_fds(t, fds, DAP_TRANSPORT_WAIT_FDS_MAX) == 2);
	CHECK(fds[0] >= 0 && fds[1] >= 0 && fds[0] != fds[1]);
	dap_transport_close(t);
}

/* A failed transport's descriptors stay ready forever without ever yielding
 * a byte, so a wait that included them would spin. */
static void test_wait_fds_are_empty_once_the_transport_has_failed(void)
{
	struct dap_transport *t = start_fake("garbage", NULL, NULL);
	int fds[DAP_TRANSPORT_WAIT_FDS_MAX];
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(dap_transport_wait_fds(t, fds, DAP_TRANSPORT_WAIT_FDS_MAX) == 0);
	dap_transport_close(t);
}

static void resolve_script_path(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');
	int dir_len = slash ? (int)(slash - argv0) + 1 : 0;

	snprintf(script_path, sizeof(script_path), "%.*sfake_dap_adapter.py",
	    dir_len, argv0);
	if (access(script_path, R_OK) == 0) {
		return;
	}
	snprintf(script_path, sizeof(script_path), "test/fake_dap_adapter.py");
}

int main(int argc, char **argv)
{
	(void)argc;
	resolve_script_path(argv[0]);
	if (access(script_path, R_OK) != 0) {
		fprintf(stderr,
		    "test_dap_transport: cannot find fake_dap_adapter.py "
		    "(tried '%s')\n",
		    script_path);
		return 1;
	}

	RUN(test_stdio_echo_round_trip);
	RUN(test_split_delivery_is_reassembled);
	RUN(test_batched_frames_come_out_of_one_read);
	RUN(test_garbage_is_a_protocol_error);
	RUN(test_huge_content_length_is_refused);
	RUN(test_truncated_frame_is_reported_as_truncation);
	RUN(test_every_kind_answers_the_ownership_table);
	RUN(test_a_spawned_adapter_gets_no_controlling_terminal);
	RUN(test_stdio_refuses_a_caller_supplied_stdin);
	RUN(test_stderr_is_a_channel_of_its_own);
	RUN(test_close_reaps_a_child_that_ignores_term);
	RUN(test_tcp_attach_close_never_signals_the_adapter);
	RUN(test_tcp_attach_shutdown_is_a_half_close_and_nothing_else);
	RUN(test_attach_to_a_rejected_host_is_a_failed_transport);
	RUN(test_half_close_waits_for_the_outbox);
	RUN(test_session_ends_on_the_response_while_the_adapter_lingers);
	RUN(test_grace_expiry_escalates_to_term);
	RUN(test_kill_backstop_reaps_an_adapter_that_ignores_term);
	RUN(test_eof_alone_ends_a_crashed_adapter);
	RUN(test_response_is_delivered_before_the_end_of_stream);
	RUN(test_wait_fds_report_the_stream_and_the_log);
	RUN(test_wait_fds_are_empty_once_the_transport_has_failed);
	return test_summary();
}
