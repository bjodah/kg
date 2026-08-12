/* Bounded, non-blocking Content-Length framing and text line channels. */

#include "framed_io.h"

#include "perf.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#endif

/* Bigger than a pipe buffer on every supported system, so a ready
 * descriptor is normally drained in one call. */
#define FRAMED_IO_READ_CHUNK 16384u

struct framed_buf {
	char *data;
	size_t len;
	size_t cap;
};

struct framed_io {
	int read_fd;
	int write_fd;
	struct framed_buf inbox;
	struct framed_buf outbox;
	size_t outbox_sent;
	size_t delivered;
	enum framed_io_error error;
	bool error_outbound;
	bool error_truncated;
	bool eof;
	bool fds_adopted;
	bool connecting;
	bool output_close_requested;
	bool output_closed;
	enum framed_io_error stop_reason;
};

struct framed_line {
	int fd;
	struct framed_buf buf;
	size_t delivered;
	size_t held;
	size_t scan_start;
	size_t scan_len;
	bool eof;
	bool scan_valid;
};

static void close_fd(int *fd)
{
	if (*fd >= 0) {
		close(*fd);
		*fd = -1;
	}
}

static void close_pair(int *read_fd, int *write_fd)
{
	if (*read_fd >= 0 && *read_fd == *write_fd) {
		*write_fd = -1;
	}
	close_fd(read_fd);
	close_fd(write_fd);
}

static int fd_prepare(int fd)
{
	int fd_flags;
	int status_flags;

	if (fd < 0) {
		return 0;
	}
	status_flags = fcntl(fd, F_GETFL, 0);
	fd_flags = fcntl(fd, F_GETFD, 0);
	if (status_flags < 0 || fd_flags < 0
	    || fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) != 0) {
		return -1;
	}
	return fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0 ? -1 : 0;
}

static int prepare_pair(int read_fd, int write_fd)
{
	if (fd_prepare(read_fd) != 0) {
		return -1;
	}
	if (write_fd != read_fd && fd_prepare(write_fd) != 0) {
		return -1;
	}
	return 0;
}

static bool buf_reserve(struct framed_buf *b, size_t extra)
{
	size_t need = b->len + extra;
	size_t cap;
	char *data;

	if (need < b->len) {
		return false;
	}
	if (need <= b->cap) {
		return true;
	}
	cap = b->cap ? b->cap : 1024;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	data = realloc(b->data, cap);
	if (!data) {
		return false;
	}
	b->data = data;
	b->cap = cap;
	return true;
}

static bool buf_append(struct framed_buf *b, const char *src, size_t len)
{
	if (!buf_reserve(b, len)) {
		return false;
	}
	memcpy(b->data + b->len, src, len);
	b->len += len;
	return true;
}

static bool buf_prepend(struct framed_buf *b, const char *src, size_t len)
{
	if (!buf_reserve(b, len)) {
		return false;
	}
	memmove(b->data + len, b->data, b->len);
	memcpy(b->data, src, len);
	b->len += len;
	return true;
}

static void buf_consume(struct framed_buf *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
}

static bool io_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

int framed_io_fail(
    struct framed_io *io, enum framed_io_error error, bool outbound)
{
	if (io->error == FRAMED_IO_OK) {
		io->error = error;
		io->error_outbound = outbound;
	}
	close_pair(&io->read_fd, &io->write_fd);
	return -1;
}

static int fail_inbound(struct framed_io *io, enum framed_io_error error)
{
	return framed_io_fail(io, error, false);
}

static bool send_blocked(struct framed_io *io);

struct framed_io *framed_io_attach_fds(int read_fd, int write_fd)
{
	struct framed_io *io = calloc(1, sizeof(*io));

	if (!io || prepare_pair(read_fd, write_fd) != 0) {
		close_pair(&read_fd, &write_fd);
		free(io);
		return NULL;
	}
	io->read_fd = read_fd;
	io->write_fd = write_fd;
	io->fds_adopted = read_fd >= 0 || write_fd >= 0;
	return io;
}

struct framed_io *framed_io_attach_socket(int fd)
{
	return framed_io_attach_fds(fd, fd);
}

struct framed_io *framed_io_new(int read_fd, int write_fd)
{
	return framed_io_attach_fds(read_fd, write_fd);
}

int framed_io_adopt_fds(struct framed_io *io, int read_fd, int write_fd)
{
	if (send_blocked(io)) {
		return -1;
	}
	if (io->fds_adopted) {
		errno = EBUSY;
		return -1;
	}
	io->fds_adopted = true;
	if (prepare_pair(read_fd, write_fd) != 0) {
		close_pair(&read_fd, &write_fd);
		return framed_io_fail(io, FRAMED_IO_ERR_IO, false);
	}
	io->read_fd = read_fd;
	io->write_fd = write_fd;
	return 0;
}

static bool loopback_host(const char *host)
{
	return host && strcmp(host, "127.0.0.1") == 0;
}

static int connect_ready(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLOUT };
	socklen_t len = sizeof(int);
	int error = 0;
	int rc;

	do {
		rc = poll(&pfd, 1, 0);
	} while (rc < 0 && errno == EINTR);
	if (rc == 0) {
		return 0;
	}
	if (rc < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0
	    || error != 0) {
		return -1;
	}
	return 1;
}

int framed_io_connect_start(
    struct framed_io *io, const char *host, unsigned short port)
{
	struct sockaddr_in addr;
	int fd;
	int rc;

	if (!io || !loopback_host(host) || port == 0) {
		errno = EINVAL;
		return -1;
	}
	if (send_blocked(io)) {
		return -1;
	}
	if (io->fds_adopted) {
		errno = EBUSY;
		return -1;
	}
	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || fd_prepare(fd) != 0) {
		close_fd(&fd);
		return framed_io_fail(io, FRAMED_IO_ERR_IO, false);
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	io->read_fd = fd;
	io->write_fd = fd;
	io->fds_adopted = true;
	rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
	if (rc != 0 && errno != EINPROGRESS && errno != EINTR) {
		return framed_io_fail(io, FRAMED_IO_ERR_IO, false);
	}
	io->connecting = true;
	return framed_io_connect_finish(io);
}

int framed_io_connect_finish(struct framed_io *io)
{
	int rc;

	if (!io || io->error != FRAMED_IO_OK) {
		return -1;
	}
	if (!io->connecting) {
		return io->fds_adopted ? 1 : 0;
	}
	rc = connect_ready(io->read_fd);
	if (rc < 0) {
		return framed_io_fail(io, FRAMED_IO_ERR_IO, false);
	}
	if (rc > 0) {
		io->connecting = false;
	}
	return rc;
}

void framed_io_close(struct framed_io *io)
{
	if (!io) {
		return;
	}
	close_pair(&io->read_fd, &io->write_fd);
	free(io->inbox.data);
	free(io->outbox.data);
	free(io);
}

static size_t header_block_end(const char *data, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len; i++) {
		if (data[i] != '\n') {
			continue;
		}
		if (data[i + 1] == '\n') {
			return i + 2;
		}
		if (i + 2 < len && data[i + 1] == '\r' && data[i + 2] == '\n') {
			return i + 3;
		}
	}
	return 0;
}

static bool next_header_line(const char *data, size_t end, size_t *pos,
    const char **line, size_t *line_len)
{
	size_t start = *pos;
	size_t eol = start;
	size_t stop;

	while (eol < end && data[eol] != '\n') {
		eol++;
	}
	stop = eol;
	if (stop > start && data[stop - 1] == '\r') {
		stop--;
	}
	*pos = (eol < end) ? eol + 1 : end;
	*line = data + start;
	*line_len = stop - start;
	return *line_len > 0;
}

static bool line_names(
    const char *line, size_t len, const char *name, size_t name_len)
{
	size_t i;
	char c;

	if (len < name_len) {
		return false;
	}
	for (i = 0; i < name_len; i++) {
		c = line[i];
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c + ('a' - 'A'));
		}
		if (c != name[i]) {
			return false;
		}
	}
	return true;
}

static bool parse_length(const char *p, size_t len, size_t *out)
{
	size_t value = 0;
	size_t digits = 0;
	size_t i = 0;

	while (i < len && (p[i] == ' ' || p[i] == '\t')) {
		i++;
	}
	for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
		if (value > (SIZE_MAX - (size_t)(p[i] - '0')) / 10) {
			return false;
		}
		value = value * 10 + (size_t)(p[i] - '0');
		digits++;
	}
	while (i < len && (p[i] == ' ' || p[i] == '\t')) {
		i++;
	}
	if (digits == 0 || i != len) {
		return false;
	}
	*out = value;
	return true;
}

static int headers_content_length(const char *data, size_t end, size_t *out)
{
	static const char name[] = "content-length:";
	const size_t name_len = sizeof(name) - 1;
	const char *line;
	size_t line_len;
	size_t pos = 0;
	bool found = false;

	while (
	    pos < end && next_header_line(data, end, &pos, &line, &line_len)) {
		if (!memchr(line, ':', line_len)) {
			return -1;
		}
		if (!line_names(line, line_len, name, name_len)) {
			continue;
		}
		if (!parse_length(line + name_len, line_len - name_len, out)) {
			return -1;
		}
		found = true;
	}
	return found ? 0 : -1;
}

static int inbox_take_message(
    struct framed_io *io, const char **body, size_t *len)
{
	size_t head = header_block_end(io->inbox.data, io->inbox.len);
	size_t length = 0;

	if (head == 0) {
		if (io->inbox.len > FRAMED_IO_MAX_HEADER_BYTES) {
			return fail_inbound(io, FRAMED_IO_ERR_TOO_LARGE);
		}
		return 0;
	}
	if (headers_content_length(io->inbox.data, head, &length) != 0) {
		return fail_inbound(io, FRAMED_IO_ERR_PROTOCOL);
	}
	if (length > FRAMED_IO_MAX_BODY_BYTES) {
		return fail_inbound(io, FRAMED_IO_ERR_TOO_LARGE);
	}
	if (io->inbox.len - head < length) {
		return 0;
	}
	*body = io->inbox.data + head;
	*len = length;
	io->delivered = head + length;
	return 1;
}

static int inbox_fill(struct framed_io *io)
{
	ssize_t n;

	if (!buf_reserve(&io->inbox, FRAMED_IO_READ_CHUNK)) {
		return fail_inbound(io, FRAMED_IO_ERR_NOMEM);
	}
	n = read(
	    io->read_fd, io->inbox.data + io->inbox.len, FRAMED_IO_READ_CHUNK);
	if (n > 0) {
		KG_PERF_INC(KG_PERF_LSP_INBOX_READ);
		io->inbox.len += (size_t)n;
		return 1;
	}
	if (n < 0 && io_would_block()) {
		return 0;
	}
	io->eof = true;
	io->stop_reason = (n == 0) ? FRAMED_IO_ERR_EOF : FRAMED_IO_ERR_IO;
	return -1;
}

static int outbox_write(struct framed_io *io)
{
	ssize_t n;

	while (io->outbox_sent < io->outbox.len) {
		n = write(io->write_fd, io->outbox.data + io->outbox_sent,
		    io->outbox.len - io->outbox_sent);
		if (n > 0) {
			io->outbox_sent += (size_t)n;
			continue;
		}
		if (n < 0 && io_would_block()) {
			return 0;
		}
		return -1;
	}
	return 0;
}

static int output_close_now(struct framed_io *io)
{
	int fd = io->write_fd;

	if (io->output_closed) {
		return 0;
	}
	if (fd >= 0 && fd == io->read_fd) {
		if (shutdown(fd, SHUT_WR) != 0 && errno != ENOTCONN) {
			return -1;
		}
		io->write_fd = -1;
	} else {
		close_fd(&io->write_fd);
	}
	io->output_closed = true;
	return 0;
}

static int output_close_if_drained(struct framed_io *io)
{
	if (!io->output_close_requested || io->outbox.len > 0) {
		return 0;
	}
	if (output_close_now(io) != 0) {
		return framed_io_fail(io, FRAMED_IO_ERR_IO, true);
	}
	return 0;
}

static int flush_ready(struct framed_io *io)
{
	void (*old_sigpipe)(int);
	int rc;

	if (io->write_fd < 0) {
		return output_close_if_drained(io);
	}
	if (io->outbox_sent < io->outbox.len) {
		old_sigpipe = signal(SIGPIPE, SIG_IGN);
		rc = outbox_write(io);
		signal(SIGPIPE, old_sigpipe);
		if (rc != 0) {
			return framed_io_fail(io, FRAMED_IO_ERR_IO, false);
		}
	}
	buf_consume(&io->outbox, io->outbox_sent);
	io->outbox_sent = 0;
	return output_close_if_drained(io);
}

int framed_io_flush(struct framed_io *io)
{
	int rc;

	if (io->error != FRAMED_IO_OK) {
		return -1;
	}
	if (!io->connecting) {
		return flush_ready(io);
	}
	rc = framed_io_connect_finish(io);
	return rc == 1 ? flush_ready(io) : rc;
}

static bool send_blocked(struct framed_io *io)
{
	if (io->error != FRAMED_IO_OK) {
		return true;
	}
	if (!io->output_close_requested) {
		return false;
	}
	errno = EPIPE;
	return true;
}

int framed_io_send(struct framed_io *io, const char *body, size_t len)
{
	char header[64];
	int header_len;

	if (send_blocked(io)) {
		return -1;
	}
	header_len = snprintf(
	    header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
	if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
		return framed_io_fail(io, FRAMED_IO_ERR_PROTOCOL, false);
	}
	if (len > FRAMED_IO_MAX_OUTBOX_BYTES
	    || io->outbox.len + (size_t)header_len + len
		> FRAMED_IO_MAX_OUTBOX_BYTES) {
		return framed_io_fail(io, FRAMED_IO_ERR_TOO_LARGE, true);
	}
	if (!buf_append(&io->outbox, header, (size_t)header_len)
	    || !buf_append(&io->outbox, body, len)) {
		return framed_io_fail(io, FRAMED_IO_ERR_NOMEM, true);
	}
	return framed_io_flush(io);
}

int framed_io_half_close_output(struct framed_io *io)
{
	if (!io || io->error != FRAMED_IO_OK) {
		return -1;
	}
	io->output_close_requested = true;
	return framed_io_flush(io);
}

bool framed_io_output_closed(const struct framed_io *io)
{
	return io && io->output_closed;
}

int framed_io_prepend(struct framed_io *io, const char *data, size_t len)
{
	if (send_blocked(io)) {
		return -1;
	}
	if (len > FRAMED_IO_MAX_OUTBOX_BYTES
	    || io->outbox.len > FRAMED_IO_MAX_OUTBOX_BYTES - len) {
		return framed_io_fail(io, FRAMED_IO_ERR_TOO_LARGE, true);
	}
	if (!buf_prepend(&io->outbox, data, len)) {
		return framed_io_fail(io, FRAMED_IO_ERR_NOMEM, true);
	}
	return 0;
}

/* Why the stream stopped, as an error.  Bytes still in the inbox at end of
 * stream are the start of a frame that never finished, which is a framing
 * failure and is classified as one -- but it is a failure of a stream that
 * stopped, not of a server that framed wrongly, and `error_truncated`
 * keeps the two apart for whoever has to put the reason into words. */
static enum framed_io_error eof_reason(struct framed_io *io)
{
	if (io->stop_reason == FRAMED_IO_ERR_EOF && io->inbox.len > 0) {
		io->error_truncated = true;
		return FRAMED_IO_ERR_PROTOCOL;
	}
	return io->stop_reason;
}

static bool input_open(const struct framed_io *io)
{
	return io->read_fd >= 0 && !io->connecting;
}

int framed_io_next_message(struct framed_io *io, const char **body, size_t *len)
{
	int rc;

	*body = NULL;
	*len = 0;
	if (io->error != FRAMED_IO_OK) {
		return -1;
	}
	buf_consume(&io->inbox, io->delivered);
	io->delivered = 0;
	if (!input_open(io)) {
		return 0;
	}
	for (;;) {
		rc = inbox_take_message(io, body, len);
		if (rc != 0) {
			return rc;
		}
		if (io->eof) {
			return fail_inbound(io, eof_reason(io));
		}
		if (inbox_fill(io) == 0) {
			return 0;
		}
	}
}

bool framed_io_failed(const struct framed_io *io)
{
	return io->error != FRAMED_IO_OK;
}

enum framed_io_error framed_io_error(const struct framed_io *io)
{
	return io->error;
}

bool framed_io_error_outbound(const struct framed_io *io)
{
	return io->error_outbound;
}

bool framed_io_error_truncated(const struct framed_io *io)
{
	return io->error_truncated;
}

size_t framed_io_pending_bytes(const struct framed_io *io)
{
	return io->outbox.len - io->outbox_sent;
}

static bool input_waitable(const struct framed_io *io)
{
	return io->error == FRAMED_IO_OK && !io->eof && input_open(io);
}

int framed_io_read_fd(const struct framed_io *io)
{
	return input_waitable(io) ? io->read_fd : -1;
}

struct framed_line *framed_line_new(int fd, bool held)
{
	struct framed_line *line = calloc(1, sizeof(*line));

	if (!line || fd_prepare(fd) != 0) {
		close_fd(&fd);
		free(line);
		return NULL;
	}
	line->fd = fd;
	line->held = held ? 0 : SIZE_MAX;
	return line;
}

void framed_line_close(struct framed_line *line)
{
	if (!line) {
		return;
	}
	close_fd(&line->fd);
	free(line->buf.data);
	free(line);
}

int framed_line_fill(struct framed_line *line)
{
	ssize_t n;

	line->scan_valid = false;
	if (line->fd < 0 || line->eof) {
		return -1;
	}
	if (!buf_reserve(&line->buf, FRAMED_IO_READ_CHUNK)) {
		line->eof = true;
		return -1;
	}
	n = read(
	    line->fd, line->buf.data + line->buf.len, FRAMED_IO_READ_CHUNK);
	if (n > 0) {
		KG_PERF_INC(KG_PERF_LSP_LINES_READ);
		line->buf.len += (size_t)n;
		return 1;
	}
	if (n < 0 && io_would_block()) {
		return 0;
	}
	line->eof = true;
	return -1;
}

static size_t line_limit(const struct framed_line *line)
{
	return (line->held < line->buf.len) ? line->held : line->buf.len;
}

static bool line_cut_at_bound(const char *base, size_t limit, const char *nl)
{
	return limit >= FRAMED_IO_MAX_LINE_BYTES
	    && (!nl || (size_t)(nl - base) > FRAMED_IO_MAX_LINE_BYTES);
}

static int line_take(
    struct framed_line *line, bool final, const char **text, size_t *len)
{
	size_t limit = line_limit(line);
	const char *nl = limit ? memchr(line->buf.data, '\n', limit) : NULL;
	size_t take;

	if (line_cut_at_bound(line->buf.data, limit, nl)) {
		take = FRAMED_IO_MAX_LINE_BYTES;
		line->delivered = take;
	} else if (nl) {
		take = (size_t)(nl - line->buf.data);
		line->delivered = take + 1;
	} else if (final && limit > 0) {
		take = limit;
		line->delivered = take;
	} else {
		return 0;
	}
	if (take > 0 && line->buf.data[take - 1] == '\r') {
		take--;
	}
	*text = line->buf.data;
	*len = take;
	return 1;
}

static void line_release(struct framed_line *line)
{
	line->scan_valid = false;
	buf_consume(&line->buf, line->delivered);
	if (line->held != SIZE_MAX) {
		line->held -= line->delivered;
	}
	line->delivered = 0;
}

void framed_line_discard_borrow(struct framed_line *line)
{
	line_release(line);
}

int framed_line_next(struct framed_line *line, const char **text, size_t *len)
{
	int rc;

	line_release(line);
	if (line->fd < 0) {
		return 0;
	}
	for (;;) {
		if (line_take(line, line->eof, text, len)) {
			return 1;
		}
		if (line->held != SIZE_MAX) {
			return 0;
		}
		rc = framed_line_fill(line);
		if (rc <= 0) {
			return (rc < 0 && line->eof)
			    ? line_take(line, true, text, len)
			    : 0;
		}
	}
}

static bool scan_line_measure(struct framed_line *line, const char *base,
    size_t avail, size_t *len, bool *newline)
{
	const char *nl = memchr(base, '\n', avail);

	*newline = nl != NULL;
	if (!*newline && !line->eof) {
		return false;
	}
	*len = *newline ? (size_t)(nl - base) : avail;
	return true;
}

bool framed_line_scan_next(
    struct framed_line *line, const char **text, size_t *len)
{
	size_t avail;
	const char *base;
	bool newline;

	line->scan_valid = false;
	if (line->held == SIZE_MAX) {
		return false;
	}
	avail = line->buf.len - line->held;
	if (avail == 0) {
		return false;
	}
	base = line->buf.data + line->held;
	if (!scan_line_measure(line, base, avail, len, &newline)) {
		return false;
	}
	line->scan_start = line->held;
	line->held += *len + (newline ? 1 : 0);
	if (*len > 0 && base[*len - 1] == '\r') {
		(*len)--;
	}
	line->scan_len = *len;
	line->scan_valid = true;
	*text = base;
	return true;
}

bool framed_line_replace_scanned_span(struct framed_line *line, size_t start,
    size_t len, const char *replacement, size_t replacement_len)
{
	size_t absolute;
	size_t tail;
	size_t extra;

	if (!line->scan_valid || start > line->scan_len
	    || len > line->scan_len - start) {
		return false;
	}
	absolute = line->scan_start + start;
	if (replacement_len > len) {
		extra = replacement_len - len;
		if (!buf_reserve(&line->buf, extra)) {
			return false;
		}
	}
	tail = line->buf.len - absolute - len;
	memmove(line->buf.data + absolute + replacement_len,
	    line->buf.data + absolute + len, tail);
	memcpy(line->buf.data + absolute, replacement, replacement_len);
	line->buf.len = line->buf.len - len + replacement_len;
	line->held = line->held - len + replacement_len;
	line->scan_len = line->scan_len - len + replacement_len;
	return true;
}

void framed_line_discard_buffer(struct framed_line *line)
{
	line->buf.len = 0;
	line->delivered = 0;
	line->held = 0;
	line->scan_start = 0;
	line->scan_len = 0;
	line->scan_valid = false;
}

void framed_line_release_hold(struct framed_line *line)
{
	line->scan_valid = false;
	line->held = SIZE_MAX;
}

size_t framed_line_buffered_bytes(const struct framed_line *line)
{
	return line->buf.len;
}

int framed_line_read_fd(const struct framed_line *line)
{
	return line->eof ? -1 : line->fd;
}
