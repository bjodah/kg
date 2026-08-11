/* LSP child, nbcode announce/connect policy, and framed I/O composition. */

#include "lsp_transport.h"

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#endif

enum lsp_transport_phase {
	LSP_PHASE_OPEN = 0,
	LSP_PHASE_ANNOUNCE,
	LSP_PHASE_CONNECTING,
};

struct lsp_transport {
	pid_t pid;
	enum lsp_wire wire;
	enum lsp_transport_phase phase;
	/* nbcode's child stdin stays separate from its socket protocol. */
	int child_in_fd;
	/* Owned here only while a non-blocking connection is in progress. */
	int sock_fd;
	struct framed_io *frames;
	struct framed_line *err;
	struct framed_line *log;
	char hash[LSP_TRANSPORT_MAX_HASH_BYTES];
	size_t hash_len;
	bool reaped;
};

static void transport_release_lines(struct lsp_transport *t)
{
	framed_line_release_hold(t->err);
	framed_line_release_hold(t->log);
}

static int transport_fail(
    struct lsp_transport *t, enum lsp_transport_error error, bool outbound)
{
	transport_release_lines(t);
	kg_close_fd(&t->child_in_fd);
	kg_close_fd(&t->sock_fd);
	return framed_io_fail(t->frames, (enum framed_io_error)error, outbound);
}

/* A framing call has already recorded the sticky reason.  Lift the log
 * hold and close descriptors that live above framed_io just as an outer
 * transport failure would. */
static int transport_frame_failed(struct lsp_transport *t)
{
	transport_release_lines(t);
	kg_close_fd(&t->child_in_fd);
	kg_close_fd(&t->sock_fd);
	return -1;
}

/* Make an in-progress socket non-blocking and close-on-exec.  Connecting
 * remains LSP's nbcode policy in this extraction; the later generic
 * connect stage moves this into framed_io's new connector. */
static int fd_own(int fd)
{
	int fd_flags = fcntl(fd, F_GETFD, 0);
	int status_flags = fcntl(fd, F_GETFL, 0);

	if (fd_flags < 0 || status_flags < 0
	    || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
		return -1;
	}
	return fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

static int connect_start(struct lsp_transport *t, unsigned short port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return transport_fail(t, LSP_TRANSPORT_ERR_IO, false);
	}
	if (fd_own(fd) != 0) {
		close(fd);
		return transport_fail(t, LSP_TRANSPORT_ERR_IO, false);
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	t->sock_fd = fd;
	t->phase = LSP_PHASE_CONNECTING;
	if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0
	    || errno == EINPROGRESS || errno == EINTR) {
		return 1;
	}
	return transport_fail(t, LSP_TRANSPORT_ERR_IO, false);
}

static int connect_status(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	socklen_t len = sizeof(int);
	int err = 0;

	if (poll(&pfd, 1, 0) <= 0) {
		return 0;
	}
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
		return -1;
	}
	return 1;
}

static int connect_finish(struct lsp_transport *t)
{
	int fd;
	int rc = connect_status(t->sock_fd);

	if (rc <= 0) {
		if (rc == 0) {
			return 0;
		}
		return transport_fail(t, LSP_TRANSPORT_ERR_IO, false);
	}
	if (framed_io_prepend(t->frames, t->hash, t->hash_len) != 0) {
		return transport_frame_failed(t);
	}
	fd = t->sock_fd;
	t->sock_fd = -1; /* adopt owns fd from this point, including failure */
	if (framed_io_adopt_fds(t->frames, fd, fd) != 0) {
		return transport_frame_failed(t);
	}
	t->phase = LSP_PHASE_OPEN;
	return 1;
}

static size_t announce_digits(
    const char *line, size_t len, size_t pos, unsigned long *out)
{
	for (; pos < len && line[pos] >= '0' && line[pos] <= '9'; pos++) {
		if (*out <= 65535u) {
			*out = *out * 10 + (unsigned long)(line[pos] - '0');
		}
	}
	return pos;
}

static size_t line_find(
    const char *line, size_t len, const char *needle, size_t needle_len)
{
	size_t i;

	if (needle_len > len) {
		return len;
	}
	for (i = 0; i + needle_len <= len; i++) {
		if (memcmp(line + i, needle, needle_len) == 0) {
			return i;
		}
	}
	return len;
}

/* The hash distinguishes nbcode's actionable announce from the earlier
 * bare-port line.  Prefix search (rather than anchoring) matches Oracle's
 * own client and accepts a logger prefix before the announce. */
static int announce_take(struct lsp_transport *t, const char *line, size_t len)
{
	static const char prefix[] = LSP_TRANSPORT_ANNOUNCE_PREFIX;
	static const char middle[] = LSP_TRANSPORT_ANNOUNCE_HASH;
	const size_t prefix_len = sizeof(prefix) - 1;
	const size_t middle_len = sizeof(middle) - 1;
	unsigned long port = 0;
	size_t start;
	size_t pos;

	start = line_find(line, len, prefix, prefix_len) + prefix_len;
	if (start > len) {
		return 0;
	}
	pos = announce_digits(line, len, start, &port);
	if (len - pos < middle_len
	    || memcmp(line + pos, middle, middle_len) != 0) {
		return 0;
	}
	if (pos == start || port == 0 || port > 65535u) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL, false);
	}
	pos += middle_len;
	if (len > pos && line[len - 1] == '\r') {
		len--;
	}
	if (len <= pos || len - pos > sizeof(t->hash)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL, false);
	}
	memcpy(t->hash, line + pos, len - pos);
	t->hash_len = len - pos;
	framed_line_release_hold(t->log);
	return connect_start(t, (unsigned short)port);
}

static int announce_scan(struct lsp_transport *t)
{
	const char *line = NULL;
	size_t len = 0;
	int rc;

	for (;;) {
		while (framed_line_scan_next(t->log, &line, &len)) {
			rc = announce_take(t, line, len);
			if (rc != 0) {
				return rc;
			}
		}
		if (framed_line_buffered_bytes(t->log)
		    > LSP_TRANSPORT_MAX_ANNOUNCE_BYTES) {
			return transport_fail(
			    t, LSP_TRANSPORT_ERR_TOO_LARGE, false);
		}
		rc = framed_line_fill(t->log);
		if (rc == 0) {
			return 0;
		}
		if (rc < 0) {
			return transport_fail(t, LSP_TRANSPORT_ERR_EOF, false);
		}
	}
}

static int transport_advance(struct lsp_transport *t)
{
	int rc;

	if (framed_io_failed(t->frames)) {
		return -1;
	}
	if (t->phase == LSP_PHASE_ANNOUNCE) {
		rc = announce_scan(t);
		if (rc <= 0) {
			return rc;
		}
	}
	if (t->phase == LSP_PHASE_CONNECTING) {
		return connect_finish(t);
	}
	return 1;
}

static struct lsp_transport *transport_alloc(void)
{
	struct lsp_transport *t = calloc(1, sizeof(*t));

	if (!t) {
		return NULL;
	}
	t->child_in_fd = -1;
	t->sock_fd = -1;
	return t;
}

static struct lsp_transport *transport_new(void)
{
	struct lsp_transport *t = transport_alloc();

	if (!t) {
		return NULL;
	}
	t->frames = framed_io_new(-1, -1);
	t->err = framed_line_new(-1, false);
	t->log = framed_line_new(-1, false);
	if (!t->frames || !t->err || !t->log) {
		framed_io_close(t->frames);
		framed_line_close(t->err);
		framed_line_close(t->log);
		free(t);
		return NULL;
	}
	return t;
}

/* Take over all three descriptors from a successful child spawn. */
static int transport_bind_child(struct lsp_transport *t, enum lsp_wire wire,
    int in_fd, int out_fd, int err_fd)
{
	framed_line_close(t->err);
	t->err = framed_line_new(err_fd, false);
	if (!t->err) {
		kg_close_fd(&in_fd);
		kg_close_fd(&out_fd);
		return -1;
	}
	if (wire == LSP_WIRE_LISTEN_HASH) {
		framed_line_close(t->log);
		t->log = framed_line_new(out_fd, true);
		if (!t->log) {
			kg_close_fd(&in_fd);
			return -1;
		}
		t->child_in_fd = in_fd;
		t->phase = LSP_PHASE_ANNOUNCE;
		return 0;
	}
	return framed_io_adopt_fds(t->frames, out_fd, in_fd);
}

struct lsp_transport *lsp_transport_start_wire(
    const struct kg_spawn_request *req, enum lsp_wire wire)
{
	struct kg_spawn_request child = *req;
	struct lsp_transport *t;
	pid_t pid = -1;
	int in_fd = -1;
	int out_fd = -1;
	int err_fd = -1;
	int saved_errno;

	child.stderr_to_output = false;
	t = transport_new();
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	if (kg_process_spawn_bidi(&child, &pid, &in_fd, &out_fd, &err_fd)
	    != 0) {
		saved_errno = errno;
		lsp_transport_close(t);
		errno = saved_errno;
		return NULL;
	}
	t->pid = pid;
	t->wire = wire;
	if (transport_bind_child(t, wire, in_fd, out_fd, err_fd) != 0) {
		saved_errno = errno;
		lsp_transport_close(t);
		errno = saved_errno;
		return NULL;
	}
	return t;
}

#ifdef KG_FUZZ
static struct lsp_transport *transport_bind_fuzz(
    struct lsp_transport *t, int out_fd)
{
	if (!t) {
		return NULL;
	}
	if (framed_io_adopt_fds(t->frames, out_fd, -1) != 0) {
		lsp_transport_close(t);
		return NULL;
	}
	t->pid = -1;
	t->reaped = true;
	return t;
}

struct lsp_transport *lsp_transport_attach_fuzz_fd(int out_fd)
{
	return transport_bind_fuzz(transport_new(), out_fd);
}
#endif

void lsp_transport_close(struct lsp_transport *t)
{
	struct kg_process_status status;

	if (!t) {
		return;
	}
	framed_io_close(t->frames);
	framed_line_close(t->err);
	framed_line_close(t->log);
	kg_close_fd(&t->child_in_fd);
	kg_close_fd(&t->sock_fd);
	if (!t->reaped && t->pid > 0) {
		kg_process_signal_group(t->pid, SIGKILL);
		kg_process_wait(t->pid, &status);
		t->reaped = true;
	}
	free(t);
}

int lsp_transport_flush(struct lsp_transport *t)
{
	int rc = transport_advance(t);

	if (rc <= 0) {
		return rc < 0 ? transport_frame_failed(t) : 0;
	}
	rc = framed_io_flush(t->frames);
	return rc < 0 ? transport_frame_failed(t) : rc;
}

int lsp_transport_send(struct lsp_transport *t, const char *body, size_t len)
{
	if (framed_io_send(t->frames, body, len) != 0) {
		return transport_frame_failed(t);
	}
	return lsp_transport_flush(t);
}

int lsp_transport_next_message(
    struct lsp_transport *t, const char **body, size_t *len)
{
	int rc = transport_advance(t);

	if (rc <= 0) {
		*body = NULL;
		*len = 0;
		return rc < 0 ? transport_frame_failed(t) : 0;
	}
	rc = framed_io_next_message(t->frames, body, len);
	return rc < 0 ? transport_frame_failed(t) : rc;
}

int lsp_transport_next_stderr_line(
    struct lsp_transport *t, const char **line, size_t *len)
{
	*line = NULL;
	*len = 0;
	/* Either channel may have supplied the last borrow; invalidate both
	 * before choosing which one supplies this call. */
	framed_line_discard_borrow(t->err);
	framed_line_discard_borrow(t->log);
	if (framed_line_next(t->err, line, len)) {
		return 1;
	}
	return framed_line_next(t->log, line, len);
}

bool lsp_transport_failed(const struct lsp_transport *t)
{
	return framed_io_failed(t->frames);
}

enum lsp_transport_error lsp_transport_error(const struct lsp_transport *t)
{
	return (enum lsp_transport_error)framed_io_error(t->frames);
}

bool lsp_transport_error_outbound(const struct lsp_transport *t)
{
	return framed_io_error_outbound(t->frames);
}

pid_t lsp_transport_pid(const struct lsp_transport *t) { return t->pid; }

bool lsp_transport_child_alive(struct lsp_transport *t)
{
	struct kg_process_status status;

	if (t->reaped || t->pid <= 0) {
		return false;
	}
	if (kg_process_reap(t->pid, &status)) {
		t->reaped = true;
		return false;
	}
	return true;
}

size_t lsp_transport_pending_bytes(const struct lsp_transport *t)
{
	return framed_io_pending_bytes(t->frames);
}

static void wait_fd_add(int fd, int *fds, int max, int *count)
{
	if (fd >= 0 && *count < max) {
		fds[(*count)++] = fd;
	}
}

int lsp_transport_wait_fds(const struct lsp_transport *t, int *fds, int max)
{
	int count = 0;

	if (framed_io_failed(t->frames)) {
		return 0;
	}
	wait_fd_add(framed_io_read_fd(t->frames), fds, max, &count);
	wait_fd_add(framed_line_read_fd(t->err), fds, max, &count);
	wait_fd_add(framed_line_read_fd(t->log), fds, max, &count);
	return count;
}
