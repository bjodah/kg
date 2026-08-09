/* ==================== LSP base-protocol transport =======================
 *
 * Frames in, frames out, over one child process's standard input and
 * standard output.  See src/lsp_transport.h for the contract; this file is
 * the whole of Stage 1 of doc/plans/2026-08-08-lsp.md, and it deliberately
 * cannot tell a JSON-RPC request from a shopping list.
 *
 * The shape is two byte buffers and two non-blocking descriptors.  Reads
 * append to the inbox and are parsed out of it a frame at a time, so a
 * header split across three reads and four messages arriving in one read
 * are the same code path.  Writes are queued in the outbox and drained as
 * the pipe allows, so a server that has stopped reading slows kg down by
 * exactly nothing until the queue's bound is reached.
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

struct lsp_transport {
	pid_t pid;
	int in_fd; /* kg writes -> child's stdin */
	int out_fd; /* kg reads <- child's stdout */
	struct lsp_buf inbox;
	struct lsp_buf outbox;
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

/* Every failure funnels here: record the first reason, close both
 * descriptors so nothing can be half-spoken to a server kg has given up
 * on, and return the -1 the public functions hand back. */
static int transport_fail(
    struct lsp_transport *t, enum lsp_transport_error error)
{
	if (t->error == LSP_TRANSPORT_OK) {
		t->error = error;
	}
	kg_close_fd(&t->in_fd);
	kg_close_fd(&t->out_fd);
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
	n = read(
	    t->out_fd, t->inbox.data + t->inbox.len, LSP_TRANSPORT_READ_CHUNK);
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

/* ------------------------------ public API ---------------------------- */

struct lsp_transport *lsp_transport_start(const struct kg_spawn_request *req)
{
	struct kg_spawn_request child = *req;
	struct lsp_transport *t;
	pid_t pid = -1;
	int in_fd = -1;
	int out_fd = -1;
	int saved_errno;

	/* Not negotiable, whatever the caller asked for: a server's stderr
	 * spliced into its stdout is one log line between a header and its
	 * body, and framing that has desynchronised never resynchronises. */
	child.stderr_to_output = false;

	t = calloc(1, sizeof(*t));
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	if (kg_process_spawn_bidi(&child, &pid, &in_fd, &out_fd) != 0) {
		saved_errno = errno;
		free(t);
		errno = saved_errno;
		return NULL;
	}
	t->pid = pid;
	t->in_fd = in_fd;
	t->out_fd = out_fd;
	return t;
}

void lsp_transport_close(struct lsp_transport *t)
{
	struct kg_process_status status;

	if (!t) {
		return;
	}
	kg_close_fd(&t->in_fd);
	kg_close_fd(&t->out_fd);
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
		n = write(t->in_fd, t->outbox.data + t->outbox_sent,
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
