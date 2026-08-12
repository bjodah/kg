#ifndef KG_FRAMED_IO_H
#define KG_FRAMED_IO_H

#include <stdbool.h>
#include <stddef.h>

/* A bounded, non-blocking Content-Length byte stream.  This layer owns
 * framing only: it never starts or reaps a child, interprets a log line,
 * or decides what the messages mean.
 *
 * framed_io_attach_fds() takes ownership of every nonnegative descriptor passed
 * to it, including on an allocation or descriptor-setup failure.  The
 * descriptors are made O_NONBLOCK and FD_CLOEXEC.  Passing the same
 * descriptor for both directions is legal and framed_io_close() closes it
 * exactly once.  A -1 side is absent; framed_io_adopt_fds() may supply the
 * sides once, which is how an owner can queue messages while it performs
 * its own announce/connect handshake.  Ownership transfers only after
 * the call verifies that both sides are still absent.  A second adoption
 * is refused without closing or otherwise taking the replacement fds;
 * after that check, adopt_fds() owns both fds including on setup failure.
 * Requesting output close is final for the object: later adopt, connect and
 * prepend calls fail with EPIPE without becoming sticky, and a refused
 * adoption leaves both descriptors owned by its caller.
 *
 * The frame body returned by framed_io_next_message() is borrowed.  It is
 * not NUL-terminated and stays valid only until the next receive or close
 * call on that framed_io.  Framing, allocation, EOF, and descriptor I/O
 * failures are sticky: the first reason wins, closes the owned protocol
 * descriptors, and makes later send/flush/receive calls return -1.
 * Closing never closes or reaps anything except the protocol descriptors
 * it owns. */

#define FRAMED_IO_MAX_HEADER_BYTES 8192u
#define FRAMED_IO_MAX_BODY_BYTES (32u * 1024u * 1024u)
#define FRAMED_IO_MAX_OUTBOX_BYTES (4u * 1024u * 1024u)
#define FRAMED_IO_MAX_LINE_BYTES 4096u

enum framed_io_error {
	FRAMED_IO_OK = 0,
	FRAMED_IO_ERR_IO,
	FRAMED_IO_ERR_EOF,
	FRAMED_IO_ERR_PROTOCOL,
	FRAMED_IO_ERR_TOO_LARGE,
	FRAMED_IO_ERR_NOMEM,
};

struct framed_io;

/* Explicit constructors for an already-open channel.  attach_fds() accepts
 * separate read/write descriptors; attach_socket() is the shared-descriptor
 * spelling whose write side can later be shut down without ending reads. */
struct framed_io *framed_io_attach_fds(int read_fd, int write_fd);
struct framed_io *framed_io_attach_socket(int fd);
/* Compatibility spelling used by the extraction's first callers. */
struct framed_io *framed_io_new(int read_fd, int write_fd);
int framed_io_adopt_fds(struct framed_io *io, int read_fd, int write_fd);

/* Begin or advance a non-blocking TCP connection.  v1 deliberately accepts
 * only the numeric loopback host `127.0.0.1`; no name lookup or remote
 * connection is hidden here.  start() consumes no descriptor on argument
 * rejection.  Once it creates a socket the framed_io owns it, including on
 * later failure.  Both calls return 1 when connected, 0 while in progress,
 * and -1 on rejection or sticky I/O failure.  Completion is confirmed with
 * SO_ERROR.  Callers may advance on their idle tick; no writable wait fd is
 * exposed by this layer yet. */
int framed_io_connect_start(
    struct framed_io *io, const char *host, unsigned short port);
int framed_io_connect_finish(struct framed_io *io);
void framed_io_close(struct framed_io *io);

/* send() returns 0 once the complete framed message has been queued, even
 * if a non-blocking write leaves bytes pending, and -1 on sticky failure.
 * flush() returns 0 both when drained and when a write would block, and -1
 * on sticky failure.  next_message() returns 1 with a borrowed body, 0
 * when no complete frame is available yet, and -1 on sticky failure. */
int framed_io_send(struct framed_io *io, const char *body, size_t len);
int framed_io_flush(struct framed_io *io);
int framed_io_next_message(
    struct framed_io *io, const char **body, size_t *len);

/* Request a graceful output half-close.  Already-queued bytes are flushed
 * first; a distinct pipe write descriptor is then close()d, while a shared
 * socket gets shutdown(SHUT_WR).  The request and repeated calls are
 * harmless.  A return of 0 means accepted, not necessarily complete:
 * output_closed() distinguishes draining from closed.  Sends after the
 * request fail with EPIPE without poisoning the input side or sticky error. */
int framed_io_half_close_output(struct framed_io *io);
bool framed_io_output_closed(const struct framed_io *io);

/* Transport-composition hook: queue unframed bytes ahead of bytes already
 * waiting to be written.  The connection owner uses this for a
 * transport-specific handshake secret; framed_io neither creates nor
 * interprets those bytes.  The total queued bytes, including the prepended
 * bytes, may not exceed FRAMED_IO_MAX_OUTBOX_BYTES.  A bound or allocation
 * failure is sticky and classified as outbound. */
int framed_io_prepend(struct framed_io *io, const char *data, size_t len);

/* Transport-composition hook: record an outer transport policy failure in
 * the same sticky state as an I/O or framing failure.  The first error
 * wins and protocol descriptors are closed. */
int framed_io_fail(
    struct framed_io *io, enum framed_io_error error, bool outbound);
bool framed_io_failed(const struct framed_io *io);
enum framed_io_error framed_io_error(const struct framed_io *io);
bool framed_io_error_outbound(const struct framed_io *io);
/* Which kind of FRAMED_IO_ERR_PROTOCOL this is: true when the framing broke
 * because the stream ended part-way through a frame, false when the bytes
 * themselves were not framing.  A server killed while writing a reply is
 * the first; a server that writes a header block with no Content-Length in
 * it is the second, and the two are not the same news for a user. */
bool framed_io_error_truncated(const struct framed_io *io);
size_t framed_io_pending_bytes(const struct framed_io *io);

/* The readable descriptor for a caller's wait set, or -1 while absent and
 * after EOF/failure.  The layer never polls or sleeps itself. */
int framed_io_read_fd(const struct framed_io *io);

/* A bounded, non-blocking text-line side channel.  framed_line_new() takes
 * ownership of fd under the same rule as framed_io_new(), makes it
 * O_NONBLOCK and FD_CLOEXEC, and accepts -1 as a permanently absent
 * channel.  EOF and I/O/allocation errors end this channel only; they
 * never fail a framed_io.
 *
 * framed_line_next() returns a line without its LF (and without a CR just
 * before that LF), or the final unterminated bytes at EOF.  Runs longer
 * than FRAMED_IO_MAX_LINE_BYTES are returned in bounded pieces.  The line
 * is borrowed until the next call on that channel or framed_line_close().
 *
 * A held channel supports an owner-specific scanner ahead of normal log
 * delivery.  scan_next() advances the hold without consuming bytes and
 * returns only physical newline-terminated lines, even though next() still
 * delivers overlong lines in bounded pieces;
 * fill() returns 1 after a read, 0 when it would block, and -1 when the
 * channel ends; release_hold() makes all scanned and future bytes visible
 * to next().  next() returns 1 with a line and 0 when no line is available
 * or the channel is exhausted.  A scanned pointer is valid only until the
 * next scan, fill, replacement, next, release_hold, or close call. */
struct framed_line;

struct framed_line *framed_line_new(int fd, bool held);
void framed_line_close(struct framed_line *line);
/* Composition hook for an owner multiplexing several line channels: end
 * this channel's outstanding borrow without reading its next line. */
void framed_line_discard_borrow(struct framed_line *line);
int framed_line_next(struct framed_line *line, const char **text, size_t *len);
bool framed_line_scan_next(
    struct framed_line *line, const char **text, size_t *len);
/* Replace bytes in the most recently scanned physical line before ordinary
 * delivery.  `start` and `len` are relative to that scan.  This lets a
 * policy owner cache a secret, then redact the retained log bytes without
 * weakening next()'s bounded-delivery rule. */
bool framed_line_replace_scanned_span(struct framed_line *line, size_t start,
    size_t len, const char *replacement, size_t replacement_len);
/* Drop retained bytes when an outer bound declares the scan hostile. */
void framed_line_discard_buffer(struct framed_line *line);
int framed_line_fill(struct framed_line *line);
void framed_line_release_hold(struct framed_line *line);
size_t framed_line_buffered_bytes(const struct framed_line *line);
int framed_line_read_fd(const struct framed_line *line);

#endif /* KG_FRAMED_IO_H */
