#ifndef KG_LSP_TRANSPORT_H
#define KG_LSP_TRANSPORT_H

#include "announce.h"
#include "framed_io.h"

#include <stddef.h>
#include <sys/types.h> /* pid_t */

/* The wire under the LSP client: one child process, with the
 * base-protocol framing of the Language Server Protocol on both
 * directions -- an ASCII header block, `Content-Length: N` among it,
 * terminated by a blank line, then exactly N bytes of body.
 *
 * There are two arrangements the frames can travel on; see `enum lsp_wire`.
 *
 * This module knows nothing about JSON, requests, or responses.  It owns
 * the child and nbcode announce/connect policy and composes framed_io for
 * the generic byte framing and line channels.  It depends on framed_io,
 * process.h and POSIX and on nothing else in the editor -- no def.h and no
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
#define LSP_TRANSPORT_MAX_HEADER_BYTES FRAMED_IO_MAX_HEADER_BYTES
#define LSP_TRANSPORT_MAX_BODY_BYTES FRAMED_IO_MAX_BODY_BYTES
#define LSP_TRANSPORT_MAX_OUTBOX_BYTES FRAMED_IO_MAX_OUTBOX_BYTES

/* The longest run of stderr bytes with no newline in it that is still
 * called a line.  A server writing a megabyte without a newline is not
 * logging, and the buffer holding it must not grow on its say-so any more
 * than the inbox may; past this the bytes so far are delivered as a line
 * and the rest becomes the next one. */
#define LSP_TRANSPORT_MAX_STDERR_LINE FRAMED_IO_MAX_LINE_BYTES

/* How the frames reach the server.
 *
 * LSP_WIRE_STDIO is the protocol's own arrangement and what every server
 * kg starts by itself speaks: the child's standard input and standard
 * output ARE the frame stream.
 *
 * LSP_WIRE_LISTEN_HASH is Oracle's nbcode (the NetBeans Java server in
 * `oracle/javavscode`), which does not speak LSP on stdio at all.  Started
 * with `--start-java-language-server=listen-hash:0` it opens a listening
 * socket, prints one line naming the port and a secret on its standard
 * output, and expects the client to connect to 127.0.0.1 at that port and
 * write the secret before the first LSP byte.  So on this wire the child's
 * stdout is an announce-then-log channel and never a frame stream, and the
 * socket is both directions of the protocol.
 *
 * The handshake is byte for byte what Oracle's own extension writes
 * (vscode/src/lsp/initializer.ts: `server.write(info[2])` on the connect
 * callback, `info` being the match of
 * /Java Language Server listening at port (\d*) with hash (.*)\n/): the
 * hash's bytes -- 128 lowercase hex characters, measured against a real
 * server -- with no newline after them and no framing around them.  A
 * server that does not like them closes the socket within a millisecond
 * and says nothing.
 *
 * That regexp is a search rather than an anchor -- it runs against
 * whatever chunk the child wrote -- so kg looks for the prefix anywhere in
 * a line too.  A server whose logger puts `INFO [nb]: ` in front of the
 * announce is announcing as far as Oracle's client is concerned, and a kg
 * that insisted on offset zero would sit out its deadline on a server
 * everything else can talk to.
 *
 * Two things about that line are measured rather than inferred, and both
 * are traps.  It is announced TWICE, and the first one has no hash on it
 * (`...listening at port N`, then `...at port N with hash H`, often in one
 * read): the hash is what makes a line the announce, and a client that
 * matched on the port alone would connect with no secret to send.  And the
 * leading words matter, because the same process announces a
 * `Java Debug Server Adapter listening at port ... with hash ...` too, and a
 * client that took that one would hand its initialize to a debugger. */
enum lsp_wire {
	LSP_WIRE_STDIO = 0,
	LSP_WIRE_LISTEN_HASH,
};

#define LSP_TRANSPORT_ANNOUNCE_PREFIX "Java Language Server listening at port "
#define LSP_TRANSPORT_ANNOUNCE_HASH " with hash "
#define LSP_TRANSPORT_DEBUG_ANNOUNCE_PREFIX                                    \
	"Java Debug Server Adapter listening at port "

enum lsp_transport_endpoint_tag {
	LSP_TRANSPORT_ENDPOINT_LANGUAGE = 1,
	LSP_TRANSPORT_ENDPOINT_JAVA_DEBUG,
};

/* Bounds on the announce phase, in the spirit of every other one here: a
 * child that talks instead of announcing must not cost unbounded memory,
 * and a hash is a short secret rather than a payload.
 *
 * What the first one is, exactly: a bound on the log channel's buffer
 * between two drains of it, and so on how far the scan may run ahead of
 * the caller.  A firehose is what trips it -- a child writing faster than
 * the scan reaches the end of the pipe, which is the one shape that would
 * otherwise grow this buffer without limit inside a single call.  A server
 * that is merely chatty does NOT: its banner arrives in pieces, and every
 * piece the scan has passed is delivered to
 * lsp_transport_next_stderr_line() and gone from the buffer, so the bound
 * sees a poll's worth at a time and nothing accumulates.
 *
 * The same bounded scanner remains active after the language socket opens,
 * because nbcode may announce its Java Debug Server Adapter later.  Its
 * endpoint is cached for a late subscriber; the language endpoint alone
 * drives this transport's connection.
 *
 * And it is not a deadline either, because the transport keeps no clock:
 * nothing here sleeps, polls or measures time.  A child that stays silent
 * forever, or one that talks for longer than anybody will wait, is the
 * *client's* deadline to notice (LSP_CLIENT_TIMEOUT_ENV, which the
 * `initialize` queued at start is already waiting on) -- and that deadline,
 * not this bound, is what a chatty server actually runs into.  How fast it
 * gets there is still set outside this file, but no longer by a clock: the
 * caller puts lsp_transport_wait_fds()' descriptors in its own wait, so a
 * child that has filled its pipe is heard as soon as it fills it and the
 * announce channel runs at the pipe's speed rather than the editor's idle
 * tick's. */
#define LSP_TRANSPORT_MAX_ANNOUNCE_BYTES (256u * 1024u)
#define LSP_TRANSPORT_MAX_HASH_BYTES KG_ANNOUNCE_MAX_SECRET_BYTES

/* Why a transport is dead.  Reported for the log and for tests; no caller
 * is expected to branch on the distinction between an I/O error and a
 * protocol one, since the remedy for all of them is the same. */
enum lsp_transport_error {
	LSP_TRANSPORT_OK = FRAMED_IO_OK,
	/* read()/write() failed, or the child closed the pipe under a
	 * write (EPIPE, ECONNRESET).  On the listen-hash wire, a socket
	 * that could not be made or that refused the connection is this
	 * too: connecting is a state, but a connect that failed is not. */
	LSP_TRANSPORT_ERR_IO = FRAMED_IO_ERR_IO,
	/* The child's stdout ended.  A server that exited, crashed, or was
	 * killed looks like this -- including one that exited before it
	 * announced a port. */
	LSP_TRANSPORT_ERR_EOF = FRAMED_IO_ERR_EOF,
	/* The bytes are not base-protocol framing: a header line with no
	 * colon, a header block with no Content-Length, or a length that is
	 * not a number.  On the listen-hash wire, an announce line with no
	 * usable port or hash in it is this too. */
	LSP_TRANSPORT_ERR_PROTOCOL = FRAMED_IO_ERR_PROTOCOL,
	/* One of the bounds above was exceeded. */
	LSP_TRANSPORT_ERR_TOO_LARGE = FRAMED_IO_ERR_TOO_LARGE,
	/* malloc()/realloc() refused. */
	LSP_TRANSPORT_ERR_NOMEM = FRAMED_IO_ERR_NOMEM,
};

struct kg_spawn_request; /* process.h; only ever pointed at here */
struct lsp_transport;

/* Spawn `req`'s command and wrap its pipes, speaking `wire`.
 * `req->stdin_fd` must be -1 (kg_process_spawn_bidi() makes the child's
 * stdin itself) and `stderr_to_output` is forced false whatever the caller
 * set: the child's stderr must never reach the stream this parses, or one
 * log line desynchronises the framing for good.  Returns NULL, with errno
 * from the spawn, if the child could not be started; a returned transport
 * always has a live child and two open descriptors.  LSP_WIRE_STDIO is
 * every server but nbcode.
 *
 * On LSP_WIRE_LISTEN_HASH the transport comes back with no socket yet and
 * is NOT a failure: it is waiting for the child's announce line, and it
 * reaches it from the caller's own poll site, in lsp_transport_flush() and
 * lsp_transport_next_message(), exactly as everything else here does.
 * Sending before then queues the bytes -- the handshake's hash is written
 * ahead of them the moment the socket is up -- and receiving says "nothing
 * yet".  The child's stdout lines, the announce among them, are delivered
 * by lsp_transport_next_stderr_line() along with its stderr. */
struct lsp_transport *lsp_transport_start_wire(
    const struct kg_spawn_request *req, enum lsp_wire wire);

#ifdef KG_FUZZ
/* Compatibility seam for fuzz-only users of the pre-extraction API: wrap
 * an already-open descriptor as a transport's read side, with no child or
 * write side.  The in-tree test/fuzz_frames.c now drives framed_io
 * directly.  Allocation failure before the framed object exists leaves
 * `out_fd` borrowed.  Once it exists, descriptor preparation failure
 * consumes and closes `out_fd`; success transfers it until
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
 * On the listen-hash wire the child's standard OUTPUT is a log too -- the
 * frames go over the socket, so everything nbcode prints, announce line
 * included, is text somebody may want to read -- and its lines come out of
 * here as well, after whatever standard error has to say.  Announce secrets
 * are replaced by `<redacted>` on this user-visible path; they exist in the
 * copy-out endpoint only.  One accessor
 * rather than two because there is exactly one thing above this that reads
 * them (the client's log hook, which puts them in *lsp-log*), and a second
 * accessor would be a second thing for every caller to remember to drain.
 * Each descriptor keeps its own buffer, so a half-written line on one
 * never lands inside a line from the other.
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
 * answering these three and refuses everything else.
 *
 * lsp_transport_error_outbound() is which SIDE the failure was on, and it
 * exists because the two sides of LSP_TRANSPORT_ERR_TOO_LARGE are two
 * different events: a body or a header block bigger than kg will hold is
 * the server sending too much, and an outbox past
 * LSP_TRANSPORT_MAX_OUTBOX_BYTES is kg queueing faster than the server
 * reads.  Nothing else here can tell them apart, and a client that reports
 * both as the server's doing blames it for kg's own queue. */
bool lsp_transport_failed(const struct lsp_transport *t);
enum lsp_transport_error lsp_transport_error(const struct lsp_transport *t);
bool lsp_transport_error_outbound(const struct lsp_transport *t);

/* The child, for a caller that owns policy: its pid, and whether a
 * WNOHANG reap says it is still there.  lsp_transport_child_alive() is the
 * only reaping this module does, and it is deliberately a question rather
 * than a duty -- who reaps, and what a dead server means, is the client
 * layer's business. */
pid_t lsp_transport_pid(const struct lsp_transport *t);
bool lsp_transport_child_alive(struct lsp_transport *t);

/* Copy a cached nbcode endpoint out only while the producing child is still
 * alive.  The query performs a WNOHANG reap itself, so a caller cannot get a
 * stale port merely because the ordinary child-alive poll has not run yet.
 * The endpoint is tagged with the producing transport's child generation;
 * replacement and reap invalidate every cached value. */
bool lsp_transport_announced_endpoint(struct lsp_transport *t,
    enum lsp_transport_endpoint_tag tag,
    struct kg_announced_endpoint *endpoint);

/* Bytes queued for the child and not yet written.  Zero means the send
 * queue is empty, which is the only reliable "the request is on its way". */
size_t lsp_transport_pending_bytes(const struct lsp_transport *t);

/* How many descriptors lsp_transport_wait_fds() may write: the protocol
 * stream and the two line channels. */
#define LSP_TRANSPORT_WAIT_FDS_MAX 3

/* The descriptors this transport is waiting to hear from, written into
 * `fds` (at most `max` of them) and counted by the return value.
 *
 * This module still keeps no clock and still polls nothing -- it says
 * WHICH descriptors a caller's own wait should include, and the caller
 * decides what to do about it.  It exists because "when the poll site
 * comes round" used to be the whole of a line channel's throughput: an
 * editor that could only wait for a keystroke drained a chatty server's
 * banner one pipe-load per idle tick, and 512 KiB of it cost six seconds
 * before the connect (doc/plans/2026-08-08-lsp.md).  A caller that puts
 * these in its wait hears the child instead.
 *
 * Only descriptors the very next poll would actually read from are
 * reported, which is what keeps such a wait from spinning: a channel that
 * has ended, and every descriptor of a transport that has failed, is left
 * out, because both stay ready forever without ever yielding a byte. */
int lsp_transport_wait_fds(const struct lsp_transport *t, int *fds, int max);

#endif /* KG_LSP_TRANSPORT_H */
