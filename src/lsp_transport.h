#ifndef KG_LSP_TRANSPORT_H
#define KG_LSP_TRANSPORT_H

#include <stddef.h>
#include <sys/types.h> /* pid_t */

/* The wire under the LSP client: one child process, spoken to over its
 * standard input and listened to over its standard output, with the
 * base-protocol framing of the Language Server Protocol on both
 * directions -- an ASCII header block, `Content-Length: N` among it,
 * terminated by a blank line, then exactly N bytes of body.
 *
 * This module knows nothing about JSON, about requests and responses, or
 * about which server is on the other end (Stages 2 and 3 of
 * doc/plans/2026-08-08-lsp.md).  It knows bytes and frames.  It depends on
 * process.h and POSIX and on nothing else in the editor -- no def.h, no
 * globals -- so it links into a test binary with the process layer alone.
 *
 * Both descriptors are non-blocking, so neither sending nor receiving ever
 * stalls the editor: a send that the pipe will not take is queued and
 * resumes on the next flush, and a receive that has no complete message
 * yet says so and returns.  Nothing here polls or sleeps; the caller's
 * poll site drives it.
 *
 * Every failure is terminal and sticky.  A transport that hits one closes
 * its descriptors and stays failed: there is no recovering a stream whose
 * framing has desynchronised, and pretending otherwise is how a client
 * ends up reading a server's log line as a response body.  The client
 * layer's answer to a failed transport is to start a new server, not to
 * repair this one.
 */

/* Bounds.  Exceeding any of them fails the transport rather than growing a
 * buffer on the say-so of the child process.
 *
 * The header block is a handful of short ASCII lines (`Content-Length`,
 * occasionally `Content-Type`); 8 KiB is orders of magnitude more than any
 * server sends and still nothing to hold.
 *
 * 32 MiB of body is chosen against real servers rather than taste: a
 * clangd `textDocument/references` reply over a large translation unit, or
 * a workspace symbol dump, runs to a few megabytes, and a full-text
 * `didOpen` for a big file goes the other way.  Beyond 32 MiB the message
 * is not a reply kg has any use for, and the number stated here is the
 * difference between a bad Content-Length costing an error and it costing
 * the editor's address space.
 *
 * The outbound queue is bounded for the mirror-image reason: a server that
 * has stopped reading its stdin must not turn kg's requests into unbounded
 * memory.  4 MiB holds several whole-document syncs of the largest file
 * anyone edits interactively. */
#define LSP_TRANSPORT_MAX_HEADER_BYTES 8192u
#define LSP_TRANSPORT_MAX_BODY_BYTES (32u * 1024u * 1024u)
#define LSP_TRANSPORT_MAX_OUTBOX_BYTES (4u * 1024u * 1024u)

/* The longest run of stderr bytes with no newline in it that is still
 * called a line.  A server writing a megabyte without a newline is not
 * logging, and the buffer holding it must not grow on its say-so any more
 * than the inbox may; past this the bytes so far are delivered as a line
 * and the rest becomes the next one. */
#define LSP_TRANSPORT_MAX_STDERR_LINE 4096u

/* Why a transport is dead.  Reported for the log and for tests; no caller
 * is expected to branch on the distinction between an I/O error and a
 * protocol one, since the remedy for all of them is the same. */
enum lsp_transport_error {
	LSP_TRANSPORT_OK = 0,
	/* read()/write() failed, or the child closed the pipe under a
	 * write (EPIPE, ECONNRESET). */
	LSP_TRANSPORT_ERR_IO,
	/* The child's stdout ended.  A server that exited, crashed, or was
	 * killed looks like this. */
	LSP_TRANSPORT_ERR_EOF,
	/* The bytes are not base-protocol framing: a header line with no
	 * colon, a header block with no Content-Length, or a length that is
	 * not a number. */
	LSP_TRANSPORT_ERR_PROTOCOL,
	/* One of the bounds above was exceeded. */
	LSP_TRANSPORT_ERR_TOO_LARGE,
	/* malloc()/realloc() refused. */
	LSP_TRANSPORT_ERR_NOMEM,
};

struct kg_spawn_request; /* process.h; only ever pointed at here */
struct lsp_transport;

/* Spawn `req`'s command and wrap its pipes.  `req->stdin_fd` must be -1
 * (kg_process_spawn_bidi() makes the child's stdin itself) and
 * `stderr_to_output` is forced false whatever the caller set: the child's
 * stderr must never reach the stream this parses, or one log line
 * desynchronises the framing for good.  Returns NULL, with errno from the
 * spawn, if the child could not be started; a returned transport always
 * has a live child and two open descriptors. */
struct lsp_transport *lsp_transport_start(const struct kg_spawn_request *req);

#ifdef KG_FUZZ
/* Fuzz-only: wrap an already-open descriptor as a transport's read side,
 * with no child process and no write side, so a harness can drive the
 * frame parser on bytes it wrote itself.  Only test/fuzz_lsp_frames.c
 * calls it and only the fuzz build defines KG_FUZZ; the editor is compiled
 * without either.  `out_fd` is taken over and closed by
 * lsp_transport_close(). */
struct lsp_transport *lsp_transport_attach_fuzz_fd(int out_fd);
#endif

/* Close the descriptors, make sure the child is gone, and free everything.
 * Killing here is not the client's shutdown policy -- a graceful
 * `shutdown`/`exit` exchange belongs to Stage 3 and happens before this --
 * it is the backstop that keeps a dropped transport from leaving a server
 * running with nobody to talk to.  NULL is accepted and ignored. */
void lsp_transport_close(struct lsp_transport *t);

/* Frame `body` (`len` bytes) and queue it, then try to flush.  Returns 0
 * when the message is queued or already written, -1 when the transport is
 * dead -- including when it died on this call, because the queue would
 * have grown past LSP_TRANSPORT_MAX_OUTBOX_BYTES or the write failed.  A
 * return of 0 does not mean the bytes have left: ask
 * lsp_transport_pending_bytes(). */
int lsp_transport_send(struct lsp_transport *t, const char *body, size_t len);

/* Write whatever the queue still holds, as much of it as the pipe takes.
 * Returns 0 (including when nothing moved because the pipe is full) or -1
 * on a dead transport.  Call it from the poll site whenever
 * lsp_transport_pending_bytes() is nonzero. */
int lsp_transport_flush(struct lsp_transport *t);

/* Pull the next complete message, reading from the child as needed.
 * Returns 1 with `*body`/`*len` set to a message body, 0 when no complete
 * message has arrived yet, and -1 when the transport is dead -- which is
 * also how end of stream is reported, with lsp_transport_error() then
 * being LSP_TRANSPORT_ERR_EOF.  Messages buffered before the child died
 * are all delivered before the -1.
 *
 * `*body` points into the transport's own buffer and is valid only until
 * the next call on this transport; it is NOT NUL-terminated, and `*len` is
 * the only thing that says where it ends.  Call in a loop until it returns
 * something other than 1. */
int lsp_transport_next_message(
    struct lsp_transport *t, const char **body, size_t *len);

/* Pull the next complete line the child wrote to its standard error,
 * reading from it as needed.  Returns 1 with `*line`/`*len` set to the
 * line's bytes without its newline, and 0 when there is no whole line to
 * give.  Call in a loop until it returns 0.
 *
 * `*line` is borrowed exactly as lsp_transport_next_message()'s body is:
 * it points into the transport's own buffer, is NOT NUL-terminated, and is
 * valid only until the next call.
 *
 * Standard error is a side channel and never fails the transport.  It has
 * its own pipe (see process.h), so nothing here can desynchronise the
 * framing on the other one; a stderr that ends, errors, or was never
 * captured at all simply has no more lines, and the protocol stream goes
 * on.  The last line before end of stream is delivered even without a
 * terminating newline, since a server that dies mid-sentence is exactly
 * the one whose sentence is worth reading. */
int lsp_transport_next_stderr_line(
    struct lsp_transport *t, const char **line, size_t *len);

/* Whether the transport has failed, and why.  A failed transport keeps
 * answering these two and refuses everything else. */
bool lsp_transport_failed(const struct lsp_transport *t);
enum lsp_transport_error lsp_transport_error(const struct lsp_transport *t);

/* The child, for a caller that owns policy: its pid, and whether a
 * WNOHANG reap says it is still there.  lsp_transport_child_alive() is the
 * only reaping this module does, and it is deliberately a question rather
 * than a duty -- who reaps, and what a dead server means, is the client
 * layer's business. */
pid_t lsp_transport_pid(const struct lsp_transport *t);
bool lsp_transport_child_alive(struct lsp_transport *t);

/* Bytes queued for the child and not yet written.  Zero means the send
 * queue is empty, which is the only reliable "the request is on its way". */
size_t lsp_transport_pending_bytes(const struct lsp_transport *t);

#endif /* KG_LSP_TRANSPORT_H */
