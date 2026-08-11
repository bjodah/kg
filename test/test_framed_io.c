#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* Direct ownership, queueing, and framing contracts for src/framed_io.c.
 * The LSP transport suite proves composition against real children; these
 * cases keep the reusable byte-stream layer independently observable. */

#include "../src/framed_io.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
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

	io = framed_io_new(read_fd, write_fd);
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
	io = framed_io_new(owned_fd, owned_fd);
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

int main(void)
{
	RUN(test_separate_pipe_ownership_and_flags);
	RUN(test_shared_socket_reads_writes_and_closes_once);
	RUN(test_second_adoption_leaves_replacement_owned_by_caller);
	RUN(test_prepend_is_before_already_framed_bytes);
	RUN(test_prepend_obeys_the_four_mib_outbox_bound);
	return test_summary();
}
