/* ==================== LSP base-protocol transport =======================
 *
 * Frames in, frames out, over one child process.  See src/lsp_transport.h
 * for the contract; this file is the whole of Stage 1 of
 * doc/plans/2026-08-08-lsp.md, and it deliberately cannot tell a JSON-RPC
 * request from a shopping list.
 *
 * The shape is two byte buffers and two non-blocking descriptors.  Reads
 * append to the inbox and are parsed out of it a frame at a time, so a
 * header split across three reads and four messages arriving in one read
 * are the same code path.  Writes are queued in the outbox and drained as
 * the pipe allows, so a server that has stopped reading slows kg down by
 * exactly nothing until the queue's bound is reached.
 *
 * The listen-hash wire changes which descriptors those are and nothing
 * else: the child's stdout becomes a line channel like its stderr, the
 * announce line on it names a port and a secret, and a non-blocking
 * connect to that port produces the descriptor the frames then use for
 * both directions.  Every step of that happens where every other step in
 * this file happens -- inside a call the caller's poll site made -- so the
 * "nothing here sleeps or polls" of the header survives it.
 */

#include "lsp_transport.h"

#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* One read() worth of pipe.  Bigger than a pipe buffer on any system kg
 * runs on, so a ready descriptor is normally drained in one call. */
#define LSP_TRANSPORT_READ_CHUNK 16384u

/* A grow-only byte buffer with an explicit length: the inbox and the
 * outbox are the same thing pointed in opposite directions. */
struct lsp_buf {
	char *data;
	size_t len;
	size_t cap;
};

/* A descriptor whose bytes are text rather than frames: its own buffer,
 * its own borrow, its own end of stream.  None of it touches `error`,
 * because a server's complaints failing has nothing to do with whether it
 * is still speaking the protocol.
 *
 * `held` is how much of the buffer belongs to the reader.  SIZE_MAX, the
 * normal state, is all of it and more: the channel reads again whenever a
 * whole line is not there yet.  A smaller number says somebody else is
 * reading this descriptor first and has only got that far -- the announce
 * scan, which must see the announce line before the log does -- and then
 * nothing past it is delivered, and nothing more is read here at all. */
struct lsp_lines {
	int fd;
	struct lsp_buf buf;
	size_t delivered;
	size_t held;
	bool eof;
};

/* Where a listen-hash transport is.  A stdio one is OPEN from the start
 * and never leaves it; the other two are states, not failures. */
enum lsp_transport_phase {
	LSP_PHASE_OPEN = 0,
	LSP_PHASE_ANNOUNCE,
	LSP_PHASE_CONNECTING,
};

struct lsp_transport {
	pid_t pid;
	enum lsp_wire wire;
	enum lsp_transport_phase phase;
	int in_fd; /* kg writes -> child's stdin */
	int out_fd; /* kg reads <- child's stdout, or -1 on the socket wire */
	int sock_fd; /* both directions on the socket wire, else -1 */
	struct lsp_buf inbox;
	struct lsp_buf outbox;
	struct lsp_lines err; /* the child's stderr */
	struct lsp_lines log; /* its stdout, on the socket wire */
	/* The secret the announce line named, kept until the connection
	 * completes and it can be put in front of the outbox. */
	char hash[LSP_TRANSPORT_MAX_HASH_BYTES];
	size_t hash_len;
	/* How much of the outbox the child has taken.  A partial write
	 * resumes here rather than re-sending. */
	size_t outbox_sent;
	/* Bytes of the inbox belonging to the message handed out by the
	 * last lsp_transport_next_message(): consumed at the head of the
	 * next call, which is what makes the borrowed body valid until
	 * exactly then and no longer. */
	size_t delivered;
	enum lsp_transport_error error;
	/* The read side is finished.  Not a failure yet: messages already
	 * buffered are delivered before the transport reports it. */
	bool eof;
	enum lsp_transport_error stop_reason;
	bool reaped;
};

static bool buf_reserve(struct lsp_buf *b, size_t extra)
{
	size_t need = b->len + extra;
	size_t cap;
	char *data;

	if (need < b->len) {
		return false; /* size_t overflow */
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

static bool buf_append(struct lsp_buf *b, const char *src, size_t len)
{
	if (!buf_reserve(b, len)) {
		return false;
	}
	memcpy(b->data + b->len, src, len);
	b->len += len;
	return true;
}

/* Put `len` bytes in FRONT of what is already queued.  One caller: the
 * listen-hash handshake, whose hash has to reach the server before the
 * `initialize` that was queued while the socket was still being made. */
static bool buf_prepend(struct lsp_buf *b, const char *src, size_t len)
{
	if (!buf_reserve(b, len)) {
		return false;
	}
	memmove(b->data + len, b->data, b->len);
	memcpy(b->data, src, len);
	b->len += len;
	return true;
}

/* Drop the first `n` bytes.  The buffers stay compact rather than growing
 * a read cursor: a message is consumed whole, so the memmove happens once
 * per message and moves only what has not been parsed yet. */
static void buf_consume(struct lsp_buf *b, size_t n)
{
	if (n >= b->len) {
		b->len = 0;
		return;
	}
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
}

/* The three errnos that mean "not now" rather than "not ever", shared by
 * both directions: a non-blocking pipe with nothing to give, a
 * non-blocking pipe that will take no more, and a signal that landed
 * mid-call.  Everything else read() or write() reports is terminal. */
static bool io_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

/* Which descriptor the frames are on.  The same one both ways on the
 * socket wire, and the child's two pipes on the stdio one; -1 while a
 * listen-hash transport is still finding its socket, which is why every
 * user of these is behind the phase check in transport_advance(). */
static int proto_read_fd(const struct lsp_transport *t)
{
	return (t->wire == LSP_WIRE_LISTEN_HASH) ? t->sock_fd : t->out_fd;
}

static int proto_write_fd(const struct lsp_transport *t)
{
	return (t->wire == LSP_WIRE_LISTEN_HASH) ? t->sock_fd : t->in_fd;
}

/* Every failure funnels here: record the first reason, close the
 * descriptors the protocol was on so nothing can be half-spoken to a
 * server kg has given up on, and return the -1 the public functions hand
 * back.  The line channels are left open on purpose -- a server that
 * explained itself and then broke did both in that order, and the
 * explanation is the half worth keeping. */
static int transport_fail(
    struct lsp_transport *t, enum lsp_transport_error error)
{
	if (t->error == LSP_TRANSPORT_OK) {
		t->error = error;
	}
	kg_close_fd(&t->in_fd);
	kg_close_fd(&t->out_fd);
	kg_close_fd(&t->sock_fd);
	return -1;
}

/* ------------------------------- parsing ------------------------------ */

/* Offset just past the blank line that ends the header block, or 0 when
 * the block has not fully arrived.  "\r\n\r\n" is the protocol's spelling;
 * a bare "\n\n" is accepted too, defensively -- it is the mistake every
 * hand-written server makes first, and accepting it costs one comparison
 * while rejecting it costs an afternoon. */
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

/* The next header line at `*pos`, terminator stripped, advancing `*pos`
 * past it.  False at the blank line that ends the block. */
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

/* Header field names are case-insensitive per the base protocol, so the
 * comparison is too; `name` is already lower case. */
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

/* A Content-Length value: optional blanks, a run of digits, optional
 * blanks, nothing else.  A value that does not fit a size_t is refused
 * here rather than wrapped -- the deliberately absurd lengths a
 * misbehaving server sends are the point of the check. */
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

/* Read the body length out of a complete header block.  Unknown fields
 * (Content-Type, and whatever a server invents next) are skipped; a line
 * with no colon is not a header at all and fails, which is what turns a
 * stream of garbage into an error instead of a wait for a blank line that
 * will never come.  Returns 0 with *out set, or -1. */
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

/* Cut one complete frame off the front of the inbox.  1 with the body
 * borrowed out, 0 when more bytes are needed, -1 when the transport has
 * just died. */
static int inbox_take_message(
    struct lsp_transport *t, const char **body, size_t *len)
{
	size_t head = header_block_end(t->inbox.data, t->inbox.len);
	size_t length = 0;

	if (head == 0) {
		if (t->inbox.len > LSP_TRANSPORT_MAX_HEADER_BYTES) {
			return transport_fail(t, LSP_TRANSPORT_ERR_TOO_LARGE);
		}
		return 0;
	}
	if (headers_content_length(t->inbox.data, head, &length) != 0) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL);
	}
	if (length > LSP_TRANSPORT_MAX_BODY_BYTES) {
		return transport_fail(t, LSP_TRANSPORT_ERR_TOO_LARGE);
	}
	if (t->inbox.len - head < length) {
		return 0;
	}
	*body = t->inbox.data + head;
	*len = length;
	t->delivered = head + length;
	return 1;
}

/* One read() into the inbox: 1 when bytes arrived, 0 when the pipe is
 * empty for now, -1 at end of stream or on an error -- both of which set
 * `eof` rather than failing, so whatever is already buffered is still
 * delivered first.  EINTR is reported as "nothing yet"; the poll site
 * calls again. */
static int inbox_fill(struct lsp_transport *t)
{
	ssize_t n;

	if (!buf_reserve(&t->inbox, LSP_TRANSPORT_READ_CHUNK)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_NOMEM);
	}
	n = read(proto_read_fd(t), t->inbox.data + t->inbox.len,
	    LSP_TRANSPORT_READ_CHUNK);
	if (n > 0) {
		t->inbox.len += (size_t)n;
		return 1;
	}
	if (n < 0 && io_would_block()) {
		return 0;
	}
	t->eof = true;
	t->stop_reason
	    = (n == 0) ? LSP_TRANSPORT_ERR_EOF : LSP_TRANSPORT_ERR_IO;
	return -1;
}

/* ------------------------------ line channels ------------------------- */

/* One read() into a line channel, with the failure policy inverted from
 * inbox_fill()'s: nothing here fails the transport, it only stops the
 * channel.  1 when bytes arrived, 0 when the pipe is empty for now, -1 at
 * end of stream, on an error, or when the descriptor was never captured. */
static int lines_fill(struct lsp_lines *ch)
{
	ssize_t n;

	if (ch->fd < 0 || ch->eof) {
		return -1;
	}
	if (!buf_reserve(&ch->buf, LSP_TRANSPORT_READ_CHUNK)) {
		ch->eof = true;
		return -1;
	}
	n = read(ch->fd, ch->buf.data + ch->buf.len, LSP_TRANSPORT_READ_CHUNK);
	if (n > 0) {
		ch->buf.len += (size_t)n;
		return 1;
	}
	if (n < 0 && io_would_block()) {
		return 0;
	}
	ch->eof = true;
	return -1;
}

/* How much of a channel's buffer may be read out of it right now. */
static size_t lines_limit(const struct lsp_lines *ch)
{
	return (ch->held < ch->buf.len) ? ch->held : ch->buf.len;
}

/* Cut one line off the front of a channel: 1 with the line borrowed out,
 * 0 when more bytes are needed.  `final` is what makes the last line
 * before end of stream a line at all -- otherwise a run with no newline in
 * it is not one yet, and waiting is the right answer. */
static int lines_take(
    struct lsp_lines *ch, bool final, const char **line, size_t *len)
{
	size_t limit = lines_limit(ch);
	const char *nl = limit ? memchr(ch->buf.data, '\n', limit) : NULL;
	size_t take;

	if (nl) {
		take = (size_t)(nl - ch->buf.data);
		ch->delivered = take + 1;
	} else if (limit >= LSP_TRANSPORT_MAX_STDERR_LINE) {
		take = LSP_TRANSPORT_MAX_STDERR_LINE;
		ch->delivered = take;
	} else if (final && limit > 0) {
		take = limit;
		ch->delivered = take;
	} else {
		return 0;
	}
	/* A server on a terminal-shaped stderr writes CRLF; the carriage
	 * return is framing, not text, and would paint as one more glyph. */
	if (take > 0 && ch->buf.data[take - 1] == '\r') {
		take--;
	}
	*line = ch->buf.data;
	*len = take;
	return 1;
}

/* Release the line the last call borrowed out.  `held` shrinks with the
 * buffer it counts into, so a channel somebody else is scanning stays in
 * step with the scan. */
static void lines_release(struct lsp_lines *ch)
{
	buf_consume(&ch->buf, ch->delivered);
	if (ch->held != SIZE_MAX) {
		ch->held -= ch->delivered;
	}
	ch->delivered = 0;
}

/* The next whole line of a channel, reading more when it may.  A channel
 * somebody else is scanning first (`held` short of the buffer) is never
 * read here: filling it would only queue bytes the scan has not reached. */
static int lines_next(struct lsp_lines *ch, const char **line, size_t *len)
{
	int rc;

	if (ch->fd < 0) {
		return 0;
	}
	for (;;) {
		if (lines_take(ch, ch->eof, line, len)) {
			return 1;
		}
		if (ch->held != SIZE_MAX) {
			return 0;
		}
		rc = lines_fill(ch);
		if (rc <= 0) {
			/* End of stream turns the tail into a line, once. */
			return (rc < 0 && ch->eof)
			    ? lines_take(ch, true, line, len)
			    : 0;
		}
	}
}

int lsp_transport_next_stderr_line(
    struct lsp_transport *t, const char **line, size_t *len)
{
	*line = NULL;
	*len = 0;
	/* The previous call's line is released here and not before, which is
	 * what the borrow in lsp_transport.h promises. */
	lines_release(&t->err);
	lines_release(&t->log);
	if (lines_next(&t->err, line, len)) {
		return 1;
	}
	return lines_next(&t->log, line, len);
}

/* ------------------------------ the socket wire ----------------------- */

/* The next unscanned line of the child's stdout, without consuming it: the
 * announce scan reads ahead of the log, and `held` is how far it has got.
 * A run of LSP_TRANSPORT_MAX_STDERR_LINE bytes with no newline in it is a
 * line here for exactly the reason it is one in lines_take() -- and by the
 * same rule, so the two never disagree about where a line ended. */
static bool announce_next_line(
    struct lsp_transport *t, const char **line, size_t *len)
{
	size_t avail = t->log.buf.len - t->log.held;
	const char *base;
	const char *nl;

	if (avail == 0) {
		return false; /* also the only state where `data` may be NULL */
	}
	base = t->log.buf.data + t->log.held;
	nl = memchr(base, '\n', avail);
	if (nl) {
		*len = (size_t)(nl - base);
		t->log.held += *len + 1;
	} else if (avail >= LSP_TRANSPORT_MAX_STDERR_LINE) {
		*len = LSP_TRANSPORT_MAX_STDERR_LINE;
		t->log.held += *len;
	} else {
		return false;
	}
	*line = base;
	return true;
}

/* Make a descriptor kg's own: non-blocking, because nothing here may ever
 * stall the editor, and close-on-exec, because every other descriptor kg
 * holds is (process.h says so, and a child inheriting a language server's
 * socket would keep it open past the server's death). */
static int fd_own(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
		return -1;
	}
	return fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? -1 : 0;
}

/* Start connecting to the port the announce named.  Loopback only: the
 * server was started by kg on this machine, and a language server is not
 * something to reach across a network by accident.  1 when the connect is
 * under way (completion is connect_finish()'s), -1 when it cannot be. */
static int connect_start(struct lsp_transport *t, unsigned short port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return transport_fail(t, LSP_TRANSPORT_ERR_IO);
	}
	if (fd_own(fd) != 0) {
		close(fd);
		return transport_fail(t, LSP_TRANSPORT_ERR_IO);
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
	return transport_fail(t, LSP_TRANSPORT_ERR_IO);
}

/* Has the connection come up?  A non-blocking connect reports itself by
 * making the descriptor writable, and SO_ERROR is the only place the
 * verdict is: a refused connection is writable too.  The poll is the
 * caller's own poll site asking, with a zero timeout, so this waits for
 * nothing.  1 connected (and the handshake queued), 0 not yet, -1 refused.
 *
 * The hash goes in FRONT of the outbox rather than being written here: it
 * is the first thing the server must read, everything queued while the
 * socket was being made comes after it, and putting it in the queue means
 * one partial-write path instead of two. */
static int connect_finish(struct lsp_transport *t)
{
	struct pollfd pfd = { .fd = t->sock_fd, .events = POLLOUT };
	socklen_t len = sizeof(int);
	int err = 0;

	if (poll(&pfd, 1, 0) <= 0) {
		return 0;
	}
	if (getsockopt(t->sock_fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0
	    || err != 0) {
		return transport_fail(t, LSP_TRANSPORT_ERR_IO);
	}
	if (!buf_prepend(&t->outbox, t->hash, t->hash_len)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_NOMEM);
	}
	t->phase = LSP_PHASE_OPEN;
	return 1;
}

/* Consume the run of digits at `pos`, returning the offset after it and
 * accumulating the value into `*out` -- saturating, so a number past the
 * port range stays past it rather than wrapping into a valid one. */
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

/* One line of the child's stdout, weighed as the announce: 0 when it is
 * not one (log noise, which is most of them), 1 when it is and the connect
 * has been started, -1 when it claims to be one and is not usable.
 *
 * The hash is what makes a line the announce, not the port.  nbcode
 * announces the same port TWICE -- once bare, `...listening at port N`,
 * and then again `...at port N with hash H`, both of which can land in one
 * read -- and the bare one is nothing to act on: there is no secret to
 * hand over, and a client that connected on it would be closed on without
 * a word.  So a line with the prefix and no ` with hash ` is skipped like
 * any other log line, and only a line that has one is held to the rest of
 * the rules. */
static int announce_take(struct lsp_transport *t, const char *line, size_t len)
{
	static const char prefix[] = LSP_TRANSPORT_ANNOUNCE_PREFIX;
	static const char middle[] = LSP_TRANSPORT_ANNOUNCE_HASH;
	const size_t prefix_len = sizeof(prefix) - 1;
	const size_t middle_len = sizeof(middle) - 1;
	unsigned long port = 0;
	size_t pos;

	if (len < prefix_len || memcmp(line, prefix, prefix_len) != 0) {
		return 0;
	}
	pos = announce_digits(line, len, prefix_len, &port);
	if (len - pos < middle_len
	    || memcmp(line + pos, middle, middle_len) != 0) {
		return 0; /* the bare announce, or something else entirely */
	}
	/* Past here the line IS the announce, so anything wrong with it is
	 * an error rather than a line to skip.  Oracle's own client matches
	 * an empty run of digits here and then connects to port 0. */
	if (pos == prefix_len || port == 0 || port > 65535u) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL);
	}
	pos += middle_len;
	/* The line's own terminator is framing, not secret: a CR here would
	 * be sent as part of the hash by Oracle's extension, whose regexp
	 * stops at the LF alone, and no server puts one in a hash. */
	if (len > pos && line[len - 1] == '\r') {
		len--;
	}
	if (len <= pos || len - pos > sizeof(t->hash)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL);
	}
	memcpy(t->hash, line + pos, len - pos);
	t->hash_len = len - pos;
	/* Read for by the log from here on: the announce line goes to the
	 * log too, as it does in every other client of this server. */
	t->log.held = SIZE_MAX;
	return connect_start(t, (unsigned short)port);
}

/* Look through the child's stdout for the announce line, reading more as
 * needed.  1 when it was found, 0 when it has not arrived yet, -1 when the
 * transport just died -- on a child that ended without announcing, or one
 * that wrote more than the announce phase is allowed to hold. */
static int announce_scan(struct lsp_transport *t)
{
	const char *line = NULL;
	size_t len = 0;
	int rc;

	for (;;) {
		while (announce_next_line(t, &line, &len)) {
			rc = announce_take(t, line, len);
			if (rc != 0) {
				return rc;
			}
		}
		if (t->log.buf.len > LSP_TRANSPORT_MAX_ANNOUNCE_BYTES) {
			return transport_fail(t, LSP_TRANSPORT_ERR_TOO_LARGE);
		}
		rc = lines_fill(&t->log);
		if (rc == 0) {
			return 0;
		}
		if (rc < 0) {
			/* Whatever it said on the way out is a line now. */
			t->log.held = SIZE_MAX;
			return transport_fail(t, LSP_TRANSPORT_ERR_EOF);
		}
	}
}

/* Get the frame stream to where the rest of this file can use it: 1 when
 * the descriptors are live, 0 while the socket wire is still finding
 * them, -1 on a transport that is dead or died here.  A stdio transport
 * answers 1 on its first branch, which is what keeps this off that path. */
static int transport_advance(struct lsp_transport *t)
{
	int rc;

	if (t->error != LSP_TRANSPORT_OK) {
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

/* ------------------------------ public API ---------------------------- */

/* A transport with nothing attached to it yet.  The -1s are what a
 * calloc() cannot express and what every close path depends on. */
static struct lsp_transport *transport_alloc(void)
{
	struct lsp_transport *t = calloc(1, sizeof(*t));

	if (!t) {
		return NULL;
	}
	t->in_fd = -1;
	t->out_fd = -1;
	t->sock_fd = -1;
	t->err.fd = -1;
	t->err.held = SIZE_MAX;
	t->log.fd = -1;
	t->log.held = SIZE_MAX;
	return t;
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

	/* Not negotiable, whatever the caller asked for: a server's stderr
	 * spliced into its stdout is one log line between a header and its
	 * body, and framing that has desynchronised never resynchronises.
	 * It gets a pipe of its own instead, which is what makes a server's
	 * complaint readable without it ever reaching the parser. */
	child.stderr_to_output = false;

	t = transport_alloc();
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	if (kg_process_spawn_bidi(&child, &pid, &in_fd, &out_fd, &err_fd)
	    != 0) {
		saved_errno = errno;
		free(t);
		errno = saved_errno;
		return NULL;
	}
	t->pid = pid;
	t->wire = wire;
	t->in_fd = in_fd;
	t->err.fd = err_fd;
	if (wire == LSP_WIRE_LISTEN_HASH) {
		/* The child's stdout is text on this wire, and the announce
		 * scan reads it before the log may: `held` at 0 is that,
		 * and it is lifted the moment the announce is found. */
		t->log.fd = out_fd;
		t->log.held = 0;
		t->phase = LSP_PHASE_ANNOUNCE;
	} else {
		t->out_fd = out_fd;
	}
	return t;
}

#ifdef KG_FUZZ
/* Fuzz-only (test/fuzz_lsp_frames.c): a transport with no child at all,
 * reading whatever the harness has already put into `out_fd`.  The parser
 * above is unreachable otherwise -- lsp_transport_start_wire() forks a server
 * per input, which costs orders of magnitude more than the parse and would
 * make the target a measurement of fork().  The descriptor becomes the
 * transport's and is closed by lsp_transport_close() like any other; there
 * is no write side and nothing to reap.  KG_FUZZ is set by FUZZ_CFLAGS
 * alone, so none of this is in the editor. */
struct lsp_transport *lsp_transport_attach_fuzz_fd(int out_fd)
{
	struct lsp_transport *t = transport_alloc();

	if (!t) {
		return NULL;
	}
	t->pid = -1;
	t->out_fd = out_fd;
	t->reaped = true;
	return t;
}
#endif

void lsp_transport_close(struct lsp_transport *t)
{
	struct kg_process_status status;

	if (!t) {
		return;
	}
	kg_close_fd(&t->in_fd);
	kg_close_fd(&t->out_fd);
	kg_close_fd(&t->sock_fd);
	kg_close_fd(&t->err.fd);
	kg_close_fd(&t->log.fd);
	if (!t->reaped && t->pid > 0) {
		/* The backstop, not the policy: Stage 3's graceful
		 * shutdown/exit exchange happens before this is reached,
		 * and a child that survived it is one nothing can talk to
		 * any more. */
		kg_process_signal_group(t->pid, SIGKILL);
		kg_process_wait(t->pid, &status);
		t->reaped = true;
	}
	free(t->inbox.data);
	free(t->outbox.data);
	free(t->err.buf.data);
	free(t->log.buf.data);
	free(t);
}

/* SIGPIPE is bracketed per flush rather than disposed of once at startup.
 * kg installs no SIGPIPE disposition of its own today, and a transport
 * that installed a process-wide one would outlive itself: every other
 * write in the editor would quietly stop dying on a broken pipe because a
 * language server was once started.  shell.c already brackets its pump
 * exactly this way, so this is the editor's existing habit rather than a
 * second one; kg is single-threaded, so the bracket is exact, and two
 * signal() calls per flush are nothing beside the write they guard.  The
 * broken pipe is then read out of write()'s EPIPE. */
static int outbox_write(struct lsp_transport *t)
{
	ssize_t n;

	while (t->outbox_sent < t->outbox.len) {
		n = write(proto_write_fd(t), t->outbox.data + t->outbox_sent,
		    t->outbox.len - t->outbox_sent);
		if (n > 0) {
			t->outbox_sent += (size_t)n;
			continue;
		}
		if (n < 0 && io_would_block()) {
			return 0; /* the pipe is full; resume next flush */
		}
		return -1;
	}
	return 0;
}

int lsp_transport_flush(struct lsp_transport *t)
{
	void (*old_sigpipe)(int);
	int rc;

	if (t->error != LSP_TRANSPORT_OK) {
		return -1;
	}
	rc = transport_advance(t);
	if (rc < 0) {
		return -1;
	}
	if (rc == 0) {
		/* No socket yet: the queue is where the bytes belong, and
		 * this is not a failure -- see lsp_transport_start_wire(). */
		return 0;
	}
	if (t->outbox_sent < t->outbox.len) {
		old_sigpipe = signal(SIGPIPE, SIG_IGN);
		rc = outbox_write(t);
		signal(SIGPIPE, old_sigpipe);
		if (rc != 0) {
			return transport_fail(t, LSP_TRANSPORT_ERR_IO);
		}
	}
	buf_consume(&t->outbox, t->outbox_sent);
	t->outbox_sent = 0;
	return 0;
}

int lsp_transport_send(struct lsp_transport *t, const char *body, size_t len)
{
	char header[64];
	int header_len;

	if (t->error != LSP_TRANSPORT_OK) {
		return -1;
	}
	header_len = snprintf(
	    header, sizeof(header), "Content-Length: %zu\r\n\r\n", len);
	if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL);
	}
	/* `len` is tested on its own first so the sum below cannot wrap. */
	if (len > LSP_TRANSPORT_MAX_OUTBOX_BYTES
	    || t->outbox.len + (size_t)header_len + len
		> LSP_TRANSPORT_MAX_OUTBOX_BYTES) {
		return transport_fail(t, LSP_TRANSPORT_ERR_TOO_LARGE);
	}
	if (!buf_append(&t->outbox, header, (size_t)header_len)
	    || !buf_append(&t->outbox, body, len)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_NOMEM);
	}
	return lsp_transport_flush(t);
}

int lsp_transport_next_message(
    struct lsp_transport *t, const char **body, size_t *len)
{
	int rc;

	*body = NULL;
	*len = 0;
	if (t->error != LSP_TRANSPORT_OK) {
		return -1;
	}
	/* The previous call's message is released here and not before, which
	 * is what the borrow in lsp_transport.h promises. */
	buf_consume(&t->inbox, t->delivered);
	t->delivered = 0;
	rc = transport_advance(t);
	if (rc <= 0) {
		return rc;
	}

	for (;;) {
		rc = inbox_take_message(t, body, len);
		if (rc != 0) {
			return rc;
		}
		if (t->eof) {
			return transport_fail(t, t->stop_reason);
		}
		if (inbox_fill(t) == 0) {
			return 0;
		}
	}
}

bool lsp_transport_failed(const struct lsp_transport *t)
{
	return t->error != LSP_TRANSPORT_OK;
}

enum lsp_transport_error lsp_transport_error(const struct lsp_transport *t)
{
	return t->error;
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
	return t->outbox.len - t->outbox_sent;
}
