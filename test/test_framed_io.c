#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* Direct ownership, queueing, and framing contracts for src/framed_io.c.
 * The LSP transport suite proves composition against real children; these
 * cases keep the reusable byte-stream layer independently observable. */

#include "../src/framed_io.h"
#include "test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define IO_DEADLINE_MS 2000

static bool fd_is_open(int fd) { return fcntl(fd, F_GETFD, 0) >= 0; }

static bool fd_is_closed(int fd)
{
	errno = 0;
	return fcntl(fd, F_GETFD, 0) < 0 && errno == EBADF;
}

static bool wait_fd(int fd, short events)
{
	struct pollfd pfd = { .fd = fd, .events = events };
	int rc;

	do {
		rc = poll(&pfd, 1, IO_DEADLINE_MS);
	} while (rc < 0 && errno == EINTR);
	return rc == 1 && (pfd.revents & events) != 0;
}

static bool read_exact(int fd, char *data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n;

		if (!wait_fd(fd, POLLIN)) {
			return false;
		}
		n = read(fd, data + off, len - off);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		off += (size_t)n;
	}
	return true;
}

static bool write_exact(int fd, const char *data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n;

		if (!wait_fd(fd, POLLOUT)) {
			return false;
		}
		n = write(fd, data + off, len - off);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		off += (size_t)n;
	}
	return true;
}

static void test_separate_pipe_ownership_and_flags(void)
{
	struct framed_io *io;
	int incoming[2];
	int outgoing[2];
	int read_fd;
	int write_fd;
	int flags;

	if (pipe(incoming) != 0) {
		CHECK(false);
		return;
	}
	if (pipe(outgoing) != 0) {
		CHECK(false);
		close(incoming[0]);
		close(incoming[1]);
		return;
	}
	read_fd = incoming[0];
	write_fd = outgoing[1];
	flags = fcntl(write_fd, F_GETFL, 0);
	CHECK(flags >= 0);
	CHECK(flags < 0 || fcntl(write_fd, F_SETFL, flags | O_APPEND) == 0);

	io = framed_io_attach_fds(read_fd, write_fd);
	CHECK(io != NULL);
	if (!io) {
		close(incoming[1]);
		close(outgoing[0]);
		return;
	}
	flags = fcntl(read_fd, F_GETFL, 0);
	CHECK(flags >= 0 && (flags & O_NONBLOCK) != 0);
	flags = fcntl(write_fd, F_GETFL, 0);
	CHECK(flags >= 0 && (flags & O_NONBLOCK) != 0);
	CHECK(flags >= 0 && (flags & O_APPEND) != 0);
	flags = fcntl(read_fd, F_GETFD, 0);
	CHECK(flags >= 0 && (flags & FD_CLOEXEC) != 0);
	flags = fcntl(write_fd, F_GETFD, 0);
	CHECK(flags >= 0 && (flags & FD_CLOEXEC) != 0);

	framed_io_close(io);
	CHECK(fd_is_closed(read_fd));
	CHECK(fd_is_closed(write_fd));
	CHECK(fd_is_open(incoming[1]));
	CHECK(fd_is_open(outgoing[0]));
	close(incoming[1]);
	close(outgoing[0]);
}

static void test_shared_socket_reads_writes_and_closes_once(void)
{
	static const char sent[] = "Content-Length: 2\r\n\r\nok";
	static const char reply[] = "Content-Length: 5\r\n\r\nreply";
	struct framed_io *io;
	const char *body = NULL;
	char wire[sizeof(sent) - 1] = { 0 };
	size_t len = 0;
	int owned_fd;
	int socket_fds[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fds) != 0) {
		CHECK(false);
		return;
	}
	owned_fd = socket_fds[0];
	io = framed_io_attach_socket(owned_fd);
	CHECK(io != NULL);
	if (!io) {
		close(socket_fds[1]);
		return;
	}
	CHECK(framed_io_send(io, "ok", 2) == 0);
	CHECK(read_exact(socket_fds[1], wire, sizeof(wire)));
	CHECK(memcmp(wire, sent, sizeof(wire)) == 0);
	CHECK(write_exact(socket_fds[1], reply, sizeof(reply) - 1));
	CHECK(framed_io_next_message(io, &body, &len) == 1);
	CHECK(len == 5);
	CHECK(body != NULL && memcmp(body, "reply", 5) == 0);

	framed_io_close(io);
	CHECK(fd_is_closed(owned_fd));
	CHECK(fd_is_open(socket_fds[1]));
	close(socket_fds[1]);
}

static void test_second_adoption_leaves_replacement_owned_by_caller(void)
{
	struct framed_io *io = framed_io_new(-1, -1);
	int initial[2];
	int replacement[2];
	int replacement_fd_flags;
	int replacement_status_flags;

	CHECK(io != NULL);
	if (!io) {
		return;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, initial) != 0) {
		CHECK(false);
		framed_io_close(io);
		return;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) != 0) {
		CHECK(false);
		close(initial[0]);
		close(initial[1]);
		framed_io_close(io);
		return;
	}
	replacement_fd_flags = fcntl(replacement[0], F_GETFD, 0);
	replacement_status_flags = fcntl(replacement[0], F_GETFL, 0);
	if (framed_io_adopt_fds(io, initial[0], initial[0]) != 0) {
		CHECK(false);
		close(initial[1]);
		close(replacement[0]);
		close(replacement[1]);
		framed_io_close(io);
		return;
	}
	errno = 0;
	CHECK(framed_io_adopt_fds(io, replacement[0], replacement[0]) == -1);
	CHECK(errno == EBUSY);
	CHECK(fd_is_open(replacement[0]));
	CHECK(fcntl(replacement[0], F_GETFD, 0) == replacement_fd_flags);
	CHECK(fcntl(replacement[0], F_GETFL, 0) == replacement_status_flags);

	close(replacement[0]);
	close(replacement[1]);
	framed_io_close(io);
	close(initial[1]);
}

static void test_prepend_is_before_already_framed_bytes(void)
{
	static const char expected[] = "hashContent-Length: 4\r\n\r\nBODY";
	struct framed_io *io = framed_io_new(-1, -1);
	char wire[sizeof(expected) - 1] = { 0 };
	int socket_fds[2];

	CHECK(io != NULL);
	if (!io) {
		return;
	}
	CHECK(framed_io_send(io, "BODY", 4) == 0);
	CHECK(framed_io_prepend(io, "hash", 4) == 0);
	CHECK(framed_io_pending_bytes(io) == sizeof(expected) - 1);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fds) != 0) {
		CHECK(false);
		framed_io_close(io);
		return;
	}
	if (framed_io_adopt_fds(io, socket_fds[0], socket_fds[0]) != 0) {
		CHECK(false);
		close(socket_fds[1]);
		framed_io_close(io);
		return;
	}
	CHECK(framed_io_flush(io) == 0);
	CHECK(read_exact(socket_fds[1], wire, sizeof(wire)));
	CHECK(memcmp(wire, expected, sizeof(wire)) == 0);

	framed_io_close(io);
	close(socket_fds[1]);
}

static void test_prepend_obeys_the_four_mib_outbox_bound(void)
{
	struct framed_io *io = framed_io_new(-1, -1);
	char *bytes = malloc(FRAMED_IO_MAX_OUTBOX_BYTES);

	CHECK(io != NULL);
	CHECK(bytes != NULL);
	if (!io || !bytes) {
		free(bytes);
		framed_io_close(io);
		return;
	}
	memset(bytes, 'x', FRAMED_IO_MAX_OUTBOX_BYTES);
	CHECK(framed_io_prepend(io, bytes, FRAMED_IO_MAX_OUTBOX_BYTES) == 0);
	CHECK(framed_io_pending_bytes(io) == FRAMED_IO_MAX_OUTBOX_BYTES);
	CHECK(framed_io_prepend(io, "x", 1) == -1);
	CHECK(framed_io_failed(io));
	CHECK(framed_io_error(io) == FRAMED_IO_ERR_TOO_LARGE);
	CHECK(framed_io_error_outbound(io));

	free(bytes);
	framed_io_close(io);
}

static void test_eof_with_partial_header_is_protocol_error(void)
{
	static const char partial[] = "Content-Len";
	struct framed_io *io;
	const char *body = NULL;
	size_t len = 0;
	int fds[2];

	if (pipe(fds) != 0) {
		CHECK(false);
		return;
	}
	io = framed_io_attach_fds(fds[0], -1);
	CHECK(io != NULL);
	if (!io) {
		close(fds[1]);
		return;
	}
	CHECK(write_exact(fds[1], partial, sizeof(partial) - 1));
	close(fds[1]);
	CHECK(framed_io_next_message(io, &body, &len) == -1);
	CHECK(framed_io_error(io) == FRAMED_IO_ERR_PROTOCOL);
	CHECK(body == NULL && len == 0);
	framed_io_close(io);
}

static void test_complete_frame_precedes_partial_body_error(void)
{
	static const char wire[] = "Content-Length: 2\r\n\r\nok"
				   "Content-Length: 5\r\n\r\nabc";
	struct framed_io *io;
	const char *body = NULL;
	size_t len = 0;
	int fds[2];

	if (pipe(fds) != 0) {
		CHECK(false);
		return;
	}
	io = framed_io_attach_fds(fds[0], -1);
	CHECK(io != NULL);
	if (!io) {
		close(fds[1]);
		return;
	}
	CHECK(write_exact(fds[1], wire, sizeof(wire) - 1));
	close(fds[1]);
	CHECK(framed_io_next_message(io, &body, &len) == 1);
	CHECK(len == 2 && body && memcmp(body, "ok", 2) == 0);
	CHECK(framed_io_next_message(io, &body, &len) == -1);
	CHECK(framed_io_error(io) == FRAMED_IO_ERR_PROTOCOL);
	framed_io_close(io);
}

static void test_complete_buffered_frame_precedes_quiet_eof(void)
{
	static const char wire[] = "Content-Length: 2\r\n\r\nok";
	struct framed_io *io;
	const char *body = NULL;
	size_t len = 0;
	int fds[2];

	if (pipe(fds) != 0) {
		CHECK(false);
		return;
	}
	io = framed_io_attach_fds(fds[0], -1);
	CHECK(io != NULL);
	if (!io) {
		close(fds[1]);
		return;
	}
	CHECK(write_exact(fds[1], wire, sizeof(wire) - 1));
	close(fds[1]);
	CHECK(framed_io_next_message(io, &body, &len) == 1);
	CHECK(len == 2 && body && memcmp(body, "ok", 2) == 0);
	CHECK(framed_io_next_message(io, &body, &len) == -1);
	CHECK(framed_io_error(io) == FRAMED_IO_ERR_EOF);
	framed_io_close(io);
}

static size_t fill_pipe(int fd)
{
	char bytes[4096];
	size_t total = 0;
	ssize_t n;

	memset(bytes, 'x', sizeof(bytes));
	for (;;) {
		n = write(fd, bytes, sizeof(bytes));
		if (n > 0) {
			total += (size_t)n;
			continue;
		}
		CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
		return total;
	}
}

static bool discard_exact(int fd, size_t len)
{
	char bytes[4096];

	while (len > 0) {
		size_t take = len < sizeof(bytes) ? len : sizeof(bytes);

		if (!read_exact(fd, bytes, take)) {
			return false;
		}
		len -= take;
	}
	return true;
}

static void test_pipe_half_close_waits_for_outbox_and_keeps_input(void)
{
	static const char sent[] = "Content-Length: 2\r\n\r\nok";
	static const char reply[] = "Content-Length: 5\r\n\r\nreply";
	struct framed_io *io;
	const char *body = NULL;
	char wire[sizeof(sent) - 1];
	size_t filled;
	size_t len = 0;
	int incoming[2];
	int outgoing[2];

	if (pipe(incoming) != 0) {
		CHECK(false);
		return;
	}
	if (pipe(outgoing) != 0) {
		CHECK(false);
		close(incoming[0]);
		close(incoming[1]);
		return;
	}
	io = framed_io_attach_fds(incoming[0], outgoing[1]);
	CHECK(io != NULL);
	if (!io) {
		close(incoming[1]);
		close(outgoing[0]);
		return;
	}
	filled = fill_pipe(outgoing[1]);
	CHECK(framed_io_send(io, "ok", 2) == 0);
	CHECK(framed_io_pending_bytes(io) == sizeof(sent) - 1);
	CHECK(framed_io_half_close_output(io) == 0);
	CHECK(!framed_io_output_closed(io));
	errno = 0;
	CHECK(framed_io_send(io, "late", 4) == -1 && errno == EPIPE);
	CHECK(!framed_io_failed(io));
	CHECK(discard_exact(outgoing[0], filled));
	CHECK(framed_io_flush(io) == 0);
	CHECK(framed_io_output_closed(io));
	CHECK(framed_io_half_close_output(io) == 0);
	CHECK(read_exact(outgoing[0], wire, sizeof(wire)));
	CHECK(memcmp(wire, sent, sizeof(wire)) == 0);
	CHECK(read(outgoing[0], wire, sizeof(wire)) == 0);
	CHECK(write_exact(incoming[1], reply, sizeof(reply) - 1));
	CHECK(framed_io_next_message(io, &body, &len) == 1);
	CHECK(len == 5 && body && memcmp(body, "reply", 5) == 0);
	CHECK(!framed_io_failed(io));
	close(incoming[1]);
	close(outgoing[0]);
	framed_io_close(io);
}

static void test_socket_half_close_is_idempotent_and_keeps_input(void)
{
	static const char sent[] = "Content-Length: 2\r\n\r\nok";
	static const char reply[] = "Content-Length: 5\r\n\r\nreply";
	struct framed_io *io;
	const char *body = NULL;
	char wire[sizeof(sent) - 1];
	size_t len = 0;
	int fds[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		CHECK(false);
		return;
	}
	io = framed_io_attach_socket(fds[0]);
	CHECK(io != NULL);
	if (!io) {
		close(fds[1]);
		return;
	}
	CHECK(framed_io_send(io, "ok", 2) == 0);
	CHECK(framed_io_half_close_output(io) == 0);
	CHECK(framed_io_output_closed(io));
	CHECK(framed_io_half_close_output(io) == 0);
	CHECK(read_exact(fds[1], wire, sizeof(wire)));
	CHECK(memcmp(wire, sent, sizeof(wire)) == 0);
	CHECK(read(fds[1], wire, sizeof(wire)) == 0);
	CHECK(write_exact(fds[1], reply, sizeof(reply) - 1));
	CHECK(framed_io_next_message(io, &body, &len) == 1);
	CHECK(len == 5 && body && memcmp(body, "reply", 5) == 0);
	CHECK(!framed_io_failed(io));
	close(fds[1]);
	framed_io_close(io);
}

static void test_blank_half_close_rejects_late_output_configuration(void)
{
	struct framed_io *io = framed_io_new(-1, -1);
	int fd_flags;
	int status_flags;
	int fds[2];

	CHECK(io != NULL);
	if (!io) {
		return;
	}
	CHECK(framed_io_half_close_output(io) == 0);
	CHECK(framed_io_output_closed(io));
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		CHECK(false);
		framed_io_close(io);
		return;
	}
	fd_flags = fcntl(fds[0], F_GETFD, 0);
	status_flags = fcntl(fds[0], F_GETFL, 0);
	errno = 0;
	CHECK(framed_io_adopt_fds(io, fds[0], fds[0]) == -1);
	CHECK(errno == EPIPE);
	CHECK(fd_is_open(fds[0]));
	CHECK(fcntl(fds[0], F_GETFD, 0) == fd_flags);
	CHECK(fcntl(fds[0], F_GETFL, 0) == status_flags);
	errno = 0;
	CHECK(framed_io_connect_start(io, "127.0.0.1", 1) == -1);
	CHECK(errno == EPIPE);
	errno = 0;
	CHECK(framed_io_prepend(io, "late", 4) == -1);
	CHECK(errno == EPIPE);
	CHECK(framed_io_pending_bytes(io) == 0);
	CHECK(!framed_io_failed(io));
	CHECK(framed_io_output_closed(io));
	close(fds[0]);
	close(fds[1]);
	framed_io_close(io);
}

static void test_held_line_replacement_grows_before_crlf(void)
{
	static const char wire[] = "prefix=x\r\nnext\n";
	static const char replacement[] = "<redacted>";
	struct framed_line *channel;
	const char *line = NULL;
	size_t len = 0;
	int fds[2];

	if (pipe(fds) != 0) {
		CHECK(false);
		return;
	}
	channel = framed_line_new(fds[0], true);
	CHECK(channel != NULL);
	if (!channel) {
		close(fds[1]);
		return;
	}
	CHECK(write_exact(fds[1], wire, sizeof(wire) - 1));
	close(fds[1]);
	CHECK(framed_line_fill(channel) == 1);
	CHECK(framed_line_scan_next(channel, &line, &len));
	CHECK(len == strlen("prefix=x"));
	CHECK(line && memcmp(line, "prefix=x", len) == 0);
	CHECK(framed_line_replace_scanned_span(channel, strlen("prefix="), 1,
	    replacement, sizeof(replacement) - 1));
	framed_line_release_hold(channel);
	CHECK(framed_line_next(channel, &line, &len) == 1);
	CHECK(len == strlen("prefix=<redacted>"));
	CHECK(line && memcmp(line, "prefix=<redacted>", len) == 0);
	CHECK(framed_line_next(channel, &line, &len) == 1);
	CHECK(len == strlen("next") && line && memcmp(line, "next", len) == 0);
	framed_line_close(channel);
}

static void check_exact_line_cut(size_t data_len, const char *ending)
{
	static const char suffix[] = "next\n";
	struct framed_line *channel;
	char *wire;
	const char *line = NULL;
	size_t ending_len = strlen(ending);
	size_t len = 0;
	int fds[2];

	wire = malloc(data_len + ending_len + sizeof(suffix) - 1);
	CHECK(wire != NULL);
	if (!wire) {
		return;
	}
	memset(wire, 'x', data_len);
	memcpy(wire + data_len, ending, ending_len);
	memcpy(wire + data_len + ending_len, suffix, sizeof(suffix) - 1);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		CHECK(false);
		free(wire);
		return;
	}
	channel = framed_line_new(fds[0], false);
	CHECK(channel != NULL);
	if (!channel) {
		close(fds[1]);
		free(wire);
		return;
	}
	CHECK(write_exact(
	    fds[1], wire, data_len + ending_len + sizeof(suffix) - 1));
	CHECK(shutdown(fds[1], SHUT_WR) == 0);
	CHECK(framed_line_next(channel, &line, &len) == 1);
	CHECK(len == data_len);
	CHECK(line && memcmp(line, wire, data_len) == 0);
	CHECK(framed_line_next(channel, &line, &len) == 1);
	CHECK(len == sizeof(suffix) - 2);
	CHECK(line && memcmp(line, suffix, sizeof(suffix) - 2) == 0);
	CHECK(framed_line_next(channel, &line, &len) == 0);
	close(fds[1]);
	framed_line_close(channel);
	free(wire);
}

/* An LF exactly at the delivery bound terminates that bounded line.  It
 * must be consumed rather than becoming a synthetic empty line on the next
 * call. */
static void test_exact_max_line_consumes_lf(void)
{
	check_exact_line_cut(FRAMED_IO_MAX_LINE_BYTES, "\n");
}

/* At the same boundary, CR is content only for delimiter accounting: it is
 * stripped from the returned line and the following LF is consumed with it.
 */
static void test_exact_max_crlf_line_consumes_delimiter(void)
{
	check_exact_line_cut(FRAMED_IO_MAX_LINE_BYTES - 1, "\r\n");
}

static int loopback_listener(unsigned short *port)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0
	    || listen(fd, 1) != 0
	    || getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
		close(fd);
		return -1;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

static void test_loopback_connect_completes_on_tick(void)
{
	static const char sent[] = "Content-Length: 2\r\n\r\nok";
	struct framed_io *io = framed_io_new(-1, -1);
	struct pollfd tick = { .fd = -1 };
	char wire[sizeof(sent) - 1];
	unsigned short port = 0;
	int listener;
	int peer;
	int rc;
	int flags;

	listener = loopback_listener(&port);
	CHECK(io != NULL && listener >= 0);
	if (!io || listener < 0) {
		framed_io_close(io);
		return;
	}
	errno = 0;
	CHECK(framed_io_connect_start(io, "localhost", port) == -1);
	CHECK(errno == EINVAL && !framed_io_failed(io));
	/* Queue-before-connect is the listen/hash composition path and remains
	 * valid until output close, even though late queueing after close is
	 * rejected above. */
	CHECK(framed_io_send(io, "ok", 2) == 0);
	rc = framed_io_connect_start(io, "127.0.0.1", port);
	CHECK(rc == 0 || rc == 1);
	/* This is the accepted wait-seam policy: no POLLOUT fd is exported;
	 * the next 100 ms editor tick explicitly advances SO_ERROR state. */
	CHECK(poll(&tick, 0, 100) == 0);
	CHECK(framed_io_connect_finish(io) == 1);
	flags = fcntl(framed_io_read_fd(io), F_GETFL, 0);
	CHECK(flags >= 0 && (flags & O_NONBLOCK) != 0);
	flags = fcntl(framed_io_read_fd(io), F_GETFD, 0);
	CHECK(flags >= 0 && (flags & FD_CLOEXEC) != 0);
	peer = accept(listener, NULL, NULL);
	CHECK(peer >= 0);
	if (peer >= 0) {
		CHECK(framed_io_flush(io) == 0);
		CHECK(read_exact(peer, wire, sizeof(wire)));
		CHECK(memcmp(wire, sent, sizeof(wire)) == 0);
		close(peer);
	}
	close(listener);
	framed_io_close(io);
}

int main(void)
{
	RUN(test_separate_pipe_ownership_and_flags);
	RUN(test_shared_socket_reads_writes_and_closes_once);
	RUN(test_second_adoption_leaves_replacement_owned_by_caller);
	RUN(test_prepend_is_before_already_framed_bytes);
	RUN(test_prepend_obeys_the_four_mib_outbox_bound);
	RUN(test_eof_with_partial_header_is_protocol_error);
	RUN(test_complete_frame_precedes_partial_body_error);
	RUN(test_complete_buffered_frame_precedes_quiet_eof);
	RUN(test_pipe_half_close_waits_for_outbox_and_keeps_input);
	RUN(test_socket_half_close_is_idempotent_and_keeps_input);
	RUN(test_blank_half_close_rejects_late_output_configuration);
	RUN(test_held_line_replacement_grows_before_crlf);
	RUN(test_exact_max_line_consumes_lf);
	RUN(test_exact_max_crlf_line_consumes_delimiter);
	RUN(test_loopback_connect_completes_on_tick);
	return test_summary();
}
