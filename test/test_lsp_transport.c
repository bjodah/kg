#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_lsp_transport.c — the LSP base-protocol transport against real
 * children (src/lsp_transport.c, Stage 1 of doc/plans/2026-08-08-lsp.md).
 *
 * There is no fake pipe here and no injected byte stream: every case forks
 * a process and talks to it over two non-blocking descriptors, because the
 * defects this module can have -- a header split across reads, a partial
 * write that never resumes, a hang on a server that says nothing -- are
 * defects of that arrangement and of nothing smaller.  Most of the
 * children are test/fake_lsp_server.py, whose modes are the misbehaviours
 * a real server can produce on demand; two are a /bin/sh printf, which is
 * how a case says exactly which bytes reach the parser.
 *
 * Nothing sleeps blind.  Every wait is pump_until_message(), which drives
 * both directions until it has an answer or a deadline passes, so a case
 * that would hang fails in a few seconds with the assertion that hung.
 */

#include "../src/lsp_transport.h"
#include "../src/process.h"
#include "test.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a loaded box or a valgrind lane never trips it, short
 * enough that a genuine hang is reported rather than waited out. */
#define PUMP_DEADLINE_SECONDS 10.0

static char script_path[1024];

static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Drive both directions until a message is delivered, the transport dies,
 * or the deadline passes.  Returns what lsp_transport_next_message()
 * returned last, or 0 on timeout -- which a caller asserting 1 or -1
 * reports as the failure it is. */
static int pump_until_message(
    struct lsp_transport *t, const char **body, size_t *len)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 }; /* 1 ms */
	int rc;

	for (;;) {
		if (lsp_transport_pending_bytes(t) > 0) {
			(void)lsp_transport_flush(t);
		}
		rc = lsp_transport_next_message(t, body, len);
		if (rc != 0) {
			return rc;
		}
		if (monotonic_seconds() >= deadline) {
			return 0;
		}
		nanosleep(&nap, NULL);
	}
}

static struct lsp_transport *start_argv(const char *const *argv)
{
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = -1,
	};

	return lsp_transport_start(&req);
}

static struct lsp_transport *start_fake(const char *mode, const char *opt,
    const char *value)
{
	const char *argv[7];
	int n = 0;

	argv[n++] = "python3";
	argv[n++] = script_path;
	argv[n++] = "--mode";
	argv[n++] = mode;
	if (opt) {
		argv[n++] = opt;
		argv[n++] = value;
	}
	argv[n] = NULL;
	return start_argv(argv);
}

/* A child that emits exactly the bytes a case names, for the framing
 * details the fake server has no mode for. */
static struct lsp_transport *start_printf(const char *format)
{
	const char *argv[4] = { "/bin/sh", "-c", NULL, NULL };
	char command[512];

	snprintf(command, sizeof(command), "printf '%s'", format);
	argv[2] = command;
	return start_argv(argv);
}

static void test_echo_round_trip(void)
{
	struct lsp_transport *t = start_fake("echo", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(lsp_transport_send(t, "{\"id\":1}", 8) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 8);
	CHECK(body && memcmp(body, "{\"id\":1}", 8) == 0);
	CHECK(!lsp_transport_failed(t));
	CHECK(lsp_transport_pending_bytes(t) == 0);
	lsp_transport_close(t);
}

/* batch mode answers three requests in one write(), so the second and
 * third messages must come out of the bytes already buffered -- asserted
 * by asking for them without pumping at all. */
static void test_several_messages_from_one_read(void)
{
	struct lsp_transport *t = start_fake("batch", "--count", "3");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(lsp_transport_send(t, "one", 3) == 0);
	CHECK(lsp_transport_send(t, "twotwo", 6) == 0);
	CHECK(lsp_transport_send(t, "three33", 7) == 0);

	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 3 && body && memcmp(body, "one", 3) == 0);
	CHECK(lsp_transport_next_message(t, &body, &len) == 1);
	CHECK(len == 6 && body && memcmp(body, "twotwo", 6) == 0);
	CHECK(lsp_transport_next_message(t, &body, &len) == 1);
	CHECK(len == 7 && body && memcmp(body, "three33", 7) == 0);
	CHECK(!lsp_transport_failed(t));
	lsp_transport_close(t);
}

/* split mode writes its answer one byte per write() with a flush after
 * each, so the header arrives split mid-token and the body a byte at a
 * time: the incremental parser is the only thing that can reassemble it. */
static void test_split_delivery_is_reassembled(void)
{
	struct lsp_transport *t = start_fake("split", "--chunk", "1");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(lsp_transport_send(t, "abcdefghij", 10) == 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 10);
	CHECK(body && memcmp(body, "abcdefghij", 10) == 0);
	lsp_transport_close(t);
}

static void test_unknown_headers_are_skipped(void)
{
	struct lsp_transport *t = start_printf(
	    "Content-Type: application/vscode-jsonrpc; charset=utf-8\\r\\n"
	    "Content-Length: 5\\r\\n\\r\\nhello");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 5 && body && memcmp(body, "hello", 5) == 0);
	lsp_transport_close(t);
}

/* Bare LF endings are not the protocol, and are tolerated anyway: the
 * first server anyone writes by hand gets this wrong. */
static void test_bare_newline_headers_are_tolerated(void)
{
	struct lsp_transport *t
	    = start_printf("Content-Length: 5\\n\\nhello");
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == 5 && body && memcmp(body, "hello", 5) == 0);
	lsp_transport_close(t);
}

static void test_garbage_is_an_error_not_a_hang(void)
{
	struct lsp_transport *t = start_fake("garbage", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(lsp_transport_failed(t));
	CHECK(lsp_transport_error(t) == LSP_TRANSPORT_ERR_PROTOCOL);
	/* A dead transport keeps refusing rather than reopening anything. */
	CHECK(lsp_transport_send(t, "x", 1) == -1);
	CHECK(lsp_transport_next_message(t, &body, &len) == -1);
	lsp_transport_close(t);
}

static void test_huge_content_length_is_refused(void)
{
	struct lsp_transport *t = start_fake("huge-header", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;
	enum lsp_transport_error err;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(lsp_transport_failed(t));
	err = lsp_transport_error(t);
	/* TOO_LARGE where the claimed length fits a size_t and exceeds the
	 * body bound, PROTOCOL where it does not even fit one. */
	CHECK(err == LSP_TRANSPORT_ERR_TOO_LARGE
	    || err == LSP_TRANSPORT_ERR_PROTOCOL);
	lsp_transport_close(t);
}

static void test_child_death_reports_eof(void)
{
	struct lsp_transport *t = start_fake("die", NULL, NULL);
	const char *body = NULL;
	size_t len = 0;

	CHECK(t != NULL);
	if (!t) {
		return;
	}
	CHECK(pump_until_message(t, &body, &len) == -1);
	CHECK(lsp_transport_error(t) == LSP_TRANSPORT_ERR_EOF);
	CHECK(lsp_transport_pid(t) > 0);
	lsp_transport_close(t);
}

/* A megabyte does not fit a pipe, so this is also the case that proves a
 * partial write resumes: the send queues what the pipe would not take, and
 * only the flushes inside the pump loop can finish it. */
static void test_large_body_round_trip(void)
{
	const size_t size = 1024u * 1024u;
	struct lsp_transport *t = start_fake("echo", NULL, NULL);
	char *payload = malloc(size);
	const char *body = NULL;
	size_t len = 0;
	size_t i;

	CHECK(t != NULL);
	CHECK(payload != NULL);
	if (!t || !payload) {
		free(payload);
		lsp_transport_close(t);
		return;
	}
	for (i = 0; i < size; i++) {
		payload[i] = (char)('a' + (i % 26));
	}
	CHECK(lsp_transport_send(t, payload, size) == 0);
	CHECK(lsp_transport_pending_bytes(t) > 0);
	CHECK(pump_until_message(t, &body, &len) == 1);
	CHECK(len == size);
	CHECK(body && memcmp(body, payload, size) == 0);
	CHECK(lsp_transport_pending_bytes(t) == 0);
	CHECK(!lsp_transport_failed(t));
	free(payload);
	lsp_transport_close(t);
}

/* The other half of the queue's contract: a send that cannot leave is
 * still queued, the transport is still healthy, and repeated flushes are
 * what drain it -- no message is read here at all. */
static void test_queued_writes_drain_on_flush(void)
{
	const size_t size = 512u * 1024u;
	struct lsp_transport *t = start_fake("echo", NULL, NULL);
	char *payload = calloc(1, size);
	double deadline;

	CHECK(t != NULL);
	CHECK(payload != NULL);
	if (!t || !payload) {
		free(payload);
		lsp_transport_close(t);
		return;
	}
	memset(payload, 'z', size);
	CHECK(lsp_transport_send(t, payload, size) == 0);
	CHECK(lsp_transport_pending_bytes(t) > 0);

	deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	while (lsp_transport_pending_bytes(t) > 0
	    && monotonic_seconds() < deadline) {
		struct timespec nap = { 0, 1000000 };
		const char *body = NULL;
		size_t len = 0;

		CHECK(lsp_transport_flush(t) == 0);
		/* The child stops reading once its own stdout pipe fills,
		 * so the queue only drains if the answers are collected
		 * too. */
		(void)lsp_transport_next_message(t, &body, &len);
		nanosleep(&nap, NULL);
	}
	CHECK(lsp_transport_pending_bytes(t) == 0);
	CHECK(!lsp_transport_failed(t));
	free(payload);
	lsp_transport_close(t);
}

/* A spawn request that names a descriptor for the child's stdin is a
 * caller bug: kg_process_spawn_bidi() makes that pipe itself. */
static void test_bidi_refuses_a_caller_supplied_stdin(void)
{
	const char *argv[2] = { "/bin/cat", NULL };
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = STDIN_FILENO,
	};
	pid_t pid = -1;
	int in_fd = -1, out_fd = -1;

	CHECK(kg_process_spawn_bidi(&req, &pid, &in_fd, &out_fd) == -1);
	CHECK(errno == EINVAL);
	CHECK(pid == -1 && in_fd == -1 && out_fd == -1);
}

/* The process-layer half on its own, with no framing in the way: write to
 * a child, read it back, close its stdin, see the end of its output, and
 * reap it. */
static void test_bidi_round_trip_against_cat(void)
{
	const char *argv[2] = { "/bin/cat", NULL };
	struct kg_spawn_request req = {
		.argv = argv,
		.stdin_fd = -1,
	};
	struct kg_process_status status;
	struct timespec nap = { 0, 1000000 };
	double deadline;
	char got[32] = { 0 };
	size_t got_len = 0;
	pid_t pid = -1;
	int in_fd = -1, out_fd = -1;
	ssize_t n;

	CHECK(kg_process_spawn_bidi(&req, &pid, &in_fd, &out_fd) == 0);
	CHECK(pid > 0 && in_fd >= 0 && out_fd >= 0);
	CHECK(write(in_fd, "ping\n", 5) == 5);

	deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	while (got_len < 5 && monotonic_seconds() < deadline) {
		n = read(out_fd, got + got_len, sizeof(got) - got_len - 1);
		if (n > 0) {
			got_len += (size_t)n;
		} else {
			nanosleep(&nap, NULL);
		}
	}
	CHECK(got_len == 5 && memcmp(got, "ping\n", 5) == 0);

	/* EOF on the child's stdin is what makes cat exit, so the read side
	 * ending is the proof the write side reached it. */
	kg_close_fd(&in_fd);
	deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	for (;;) {
		n = read(out_fd, got, sizeof(got));
		if (n == 0 || monotonic_seconds() >= deadline) {
			break;
		}
		nanosleep(&nap, NULL);
	}
	CHECK(n == 0);
	kg_close_fd(&out_fd);
	CHECK(kg_process_wait(pid, &status) == 0);
	CHECK(status.exited && status.exit_code == 0);
}

/* Where test/fake_lsp_server.py is, given how this binary was invoked:
 * beside it in test/ for `make check` and for a hand-run `valgrind
 * ./test/test_lsp_transport`, with the repo-relative path as the fallback
 * for a runner that renames the binary. */
static void resolve_script_path(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');
	int dir_len = slash ? (int)(slash - argv0) + 1 : 0;

	snprintf(script_path, sizeof(script_path), "%.*sfake_lsp_server.py",
	    dir_len, argv0);
	if (access(script_path, R_OK) == 0) {
		return;
	}
	snprintf(script_path, sizeof(script_path), "test/fake_lsp_server.py");
}

/* python3 is a hard requirement of this repo's toolchain (the PTY harness
 * and every ratchet script need it), so a missing one is reported as the
 * broken box it is rather than skipped around. */
static bool python3_on_path(void)
{
	const char *path = getenv("PATH");
	char candidate[1024];
	const char *start;
	const char *colon;
	size_t len;

	if (!path) {
		return false;
	}
	for (start = path; *start; start = colon + (*colon == ':')) {
		colon = strchr(start, ':');
		if (!colon) {
			colon = start + strlen(start);
		}
		len = (size_t)(colon - start);
		if (len == 0 || len + 9 >= sizeof(candidate)) {
			continue;
		}
		snprintf(candidate, sizeof(candidate), "%.*s/python3",
		    (int)len, start);
		if (access(candidate, X_OK) == 0) {
			return true;
		}
	}
	return false;
}

int main(int argc, char **argv)
{
	(void)argc;
	resolve_script_path(argv[0]);
	if (access(script_path, R_OK) != 0) {
		fprintf(stderr,
		    "test_lsp_transport: cannot find fake_lsp_server.py "
		    "(tried '%s')\n",
		    script_path);
		return 1;
	}
	if (!python3_on_path()) {
		fprintf(stderr,
		    "test_lsp_transport: python3 is not on PATH; this "
		    "repo's toolchain requires it\n");
		return 1;
	}

	RUN(test_bidi_refuses_a_caller_supplied_stdin);
	RUN(test_bidi_round_trip_against_cat);
	RUN(test_echo_round_trip);
	RUN(test_several_messages_from_one_read);
	RUN(test_split_delivery_is_reassembled);
	RUN(test_unknown_headers_are_skipped);
	RUN(test_bare_newline_headers_are_tolerated);
	RUN(test_garbage_is_an_error_not_a_hang);
	RUN(test_huge_content_length_is_refused);
	RUN(test_child_death_reports_eof);
	RUN(test_large_body_round_trip);
	RUN(test_queued_writes_drain_on_flush);
	return test_summary();
}
