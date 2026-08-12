#ifndef KG_DAP_TRANSPORT_H
#define KG_DAP_TRANSPORT_H

#include "announce.h"
#include "framed_io.h"

#include <stddef.h>
#include <sys/types.h> /* pid_t */

/* The wire under the DAP client: one debug adapter, reached either as a
 * child speaking the base protocol on its own stdin/stdout or as a TCP
 * connection to an adapter somebody else started.  The framing is byte for
 * byte the Language Server Protocol's, so this module composes
 * src/framed_io.c for it and owns nothing about JSON, requests, sessions or
 * the editor -- it depends on framed_io, process.h and POSIX, and links
 * into a test binary with the process layer alone.
 *
 * The bounds are framed_io's own (FRAMED_IO_MAX_BODY_BYTES and friends);
 * this layer adds none, because nothing here buffers anything framed_io
 * does not.
 *
 * What it does own, and what src/lsp_transport.c has no equivalent of, is
 * the ownership question and the end-of-session ladder.
 *
 * OWNERSHIP.  A close path must never signal a child it did not start, and
 * every child it did start must be reaped on every exit.  The four
 * arrangements, of which v1 implements the first two:
 *
 *   kind         | child owner | protocol fds          | close backstop
 *   -------------+-------------+-----------------------+---------------------
 *   stdio        | DAP         | child stdin/stdout    | close, TERM/KILL
 *                |             |                       | the group, reap
 *   tcp attach   | none        | one socket            | shutdown/close only
 *   spawn-port   | DAP         | socket + child log    | close socket, drain
 *                |             | pipes                 | logs, kill/reap
 *   lsp-sibling  | LSP         | one DAP socket        | close that socket
 *   (subplan 03) |             |                       | only
 *
 * `enum dap_transport_kind` carries all four rows because the table is the
 * contract, not the implementation status, and
 * dap_transport_kind_owns_child() answers for all four so the question is
 * asked of data rather than re-derived per call site.
 *
 * THE SPAWN-PORT ROW is the one with a state machine rather than a
 * constructor that is done when it returns.  `dlv dap` is TCP-only -- no
 * stdio mode exists -- and it prints the port it chose on its own standard
 * output, so kg spawns it, scrapes that one line, connects, and only then
 * has a protocol stream:
 *
 *   SPAWNING -> WAIT_ANNOUNCE -> CONNECTING -> OPEN
 *                            any failure -> the sticky error, then close
 *
 * Before the connect, stdout is scanned incrementally and still drained to
 * a bounded log destination; afterwards it stays a log channel and never
 * becomes the frame stream.  Standard error is always drained, and on this
 * kind it is also where a debuggee's own output arrives (see
 * dap_transport_set_stderr_bytes()).  End of the child's stdout before the
 * announce is a startup failure; end of it after the connect is not the
 * socket's end.  Every failure and cancel path closes the socket and the
 * log descriptors, signals the owned process group and reaps it.
 *
 * kg never globs or deletes `__debug_bin*`: delve owns its build
 * artefact's cleanup, and an editor that deleted files matching a pattern
 * in a user's directory would be a worse bug than a stale binary.
 *
 * THE END-OF-SESSION LADDER [M-8].  A debug adapter that has answered
 * `disconnect` may exit at once (lldb-dap) or stay alive indefinitely
 * (debugpy: measured 30 s and still healthy).  So end of stream is a
 * terminal edge like any other transport failure, but it is NOT the edge
 * the session may wait for: the client ends a session on the disconnect
 * response, and this ladder is what makes that safe, because it is what
 * gets rid of the adapter afterwards.  It also ends a session on EOF alone,
 * which is what an adapter that crashed looks like.
 *
 * The rungs are dap_transport_begin_shutdown() and repeated
 * dap_transport_shutdown_poll():
 *
 *   FLUSHING   the outbox drains first -- a `disconnect` still queued is
 *              the whole point of the exchange, so the write side is not
 *              closed under it.
 *   GRACE      the output half-close has happened (close() of the child's
 *              stdin for a pipe, shutdown(SHUT_WR) for a socket -- the
 *              spelling is framed_io's, the timing is ours), and the grace
 *              deadline starts HERE rather than at begin_shutdown, so a
 *              slow flush never eats the adapter's time to exit.
 *   SIGNALLED  the grace deadline passed with the owned child still there:
 *              SIGTERM to its process group.
 *   KILLED     the kill deadline passed too: SIGKILL to the group.
 *   DONE       nothing is left to wait for -- the owned child is reaped, or
 *              there was never one to reap, which is where a tcp-attach
 *              transport goes straight from GRACE.
 *
 * Reaping is poll-driven with waitpid(WNOHANG) throughout (process.h), the
 * house design: kg installs no SIGCHLD handler.
 *
 * Unlike src/lsp_transport.c, this module keeps a clock.  It has to: the
 * grace and kill deadlines are durations measured from a rung it entered
 * itself, and no caller can time a step it cannot see the start of.  It
 * still never sleeps and never polls -- shutdown_poll() reads
 * CLOCK_MONOTONIC and returns -- so the caller's poll site is still the
 * only thing that drives it.
 *
 * Every framing failure is terminal and sticky, framed_io's model
 * unchanged: the first reason wins, the protocol descriptors close, and a
 * failed transport is replaced rather than repaired.
 */

/* Lifecycle deadlines, in milliseconds, defined per step rather than as one
 * "adapter timeout": these are six different questions about six different
 * pieces of machinery, and a single number is wrong for at least five of
 * them.  They live together here so the ladder is legible in one place;
 * which layer ENFORCES each is named with it.
 *
 * The numbers are sized against the measured adapters (parent plan, M-15):
 * debugpy is ~160 ms to initialize and a flat ~41 ms per round trip,
 * lldb-dap 8 ms and 0-7 ms.  Every value below is at least an order of
 * magnitude above that, because the lane that matters is the sanitizer one
 * on a loaded box, and a deadline that fires there is a flake rather than a
 * diagnosis.
 *
 * DAP_TRANSPORT_NO_DEADLINE is not a large number, it is the absence of
 * one, and exactly one request gets it: launch/attach [M-1].  Its response
 * has no fixed position -- lldb-dap answers before the `initialized` event,
 * debugpy after the `configurationDone` response -- and a debuggee may take
 * as long as it likes to start.  What replaces the deadline is
 * cancellation: the session stays in a visible "starting" state that C-g
 * ends, and C-g ends it by closing this transport, which is why cancelling
 * never depends on a launch response arriving. */
#define DAP_TRANSPORT_NO_DEADLINE 0u
/* Spawn to first byte, or to a port announce on the kinds that scrape one.
 * Enforced by the announce scan; the kinds with nothing to announce never
 * start this clock.  Measured, `dlv dap` reaches "listening" in 4-10 ms, so
 * this is three orders of magnitude of room -- it is the deadline for a
 * child that will never announce (one told to log elsewhere, one that is
 * not an adapter at all), not for a slow one. */
#define DAP_TRANSPORT_SPAWN_TIMEOUT_MS 10000u
/* connect() to a listening adapter.  Enforced here: a nonblocking connect
 * that neither completes nor is refused would otherwise wedge a session
 * with no descriptor ever becoming readable to notice it by. */
#define DAP_TRANSPORT_CONNECT_TIMEOUT_MS 5000u
/* The `initialize` request, which is the first thing that proves an adapter
 * is an adapter.  Enforced by the client (stage 3). */
#define DAP_TRANSPORT_INITIALIZE_TIMEOUT_MS 20000u
/* Any other request except launch/attach.  Enforced by the client's pending
 * table (stage 3). */
#define DAP_TRANSPORT_REQUEST_TIMEOUT_MS 15000u
/* Two windows of the ladder, not one: how long the outbox has to drain, and
 * then how long a half-closed adapter has to exit by itself.  Both enforced
 * here. */
#define DAP_TRANSPORT_SHUTDOWN_TIMEOUT_MS 2000u
/* SIGTERM to exit, and then SIGKILL to be reaped.  Enforced here. */
#define DAP_TRANSPORT_KILL_TIMEOUT_MS 1000u

/* How many bytes of a child's stdout may be retained while the scan has not
 * found the announce yet.  A bound on the scan and not on the log: the
 * bytes the scan has passed are delivered to the log destination as usual,
 * and this is what the scan may hold ONTO -- a child that writes megabytes
 * of banner without ever announcing is refused rather than buffered. */
#define DAP_TRANSPORT_MAX_ANNOUNCE_BYTES (64u * 1024u)

/* The four deadlines this module enforces, overridable per transport so a
 * test can drive the ladder without waiting one out.  Zero means "already
 * expired", which is a legal policy and the one the tests use. */
struct dap_transport_deadlines {
	unsigned spawn_ms;
	unsigned connect_ms;
	unsigned shutdown_ms;
	unsigned kill_ms;
};

enum dap_transport_kind {
	DAP_TRANSPORT_STDIO = 0,
	DAP_TRANSPORT_TCP_ATTACH,
	DAP_TRANSPORT_SPAWN_PORT,
	DAP_TRANSPORT_LSP_SIBLING,
};

enum dap_transport_shutdown_state {
	DAP_TRANSPORT_SHUTDOWN_IDLE = 0,
	DAP_TRANSPORT_SHUTDOWN_FLUSHING,
	DAP_TRANSPORT_SHUTDOWN_GRACE,
	DAP_TRANSPORT_SHUTDOWN_SIGNALLED,
	DAP_TRANSPORT_SHUTDOWN_KILLED,
	DAP_TRANSPORT_SHUTDOWN_DONE,
};

struct kg_spawn_request; /* process.h; only ever pointed at here */
struct dap_transport;

/* Whether a transport of this kind started the process on the other end,
 * and so whether its close path may signal and reap one.  The ownership
 * table above, as data: every kind answers, including the two with no
 * constructor yet. */
bool dap_transport_kind_owns_child(enum dap_transport_kind kind);

/* Spawn `req`'s adapter and speak the protocol on its stdin and stdout.
 * `req->stdin_fd` must be -1 (kg_process_spawn_bidi() makes that pipe
 * itself, and a descriptor there is refused with EINVAL rather than
 * ignored), and `stderr_to_output` is forced false whatever the caller set:
 * one log line spliced into the frame stream desynchronises the framing for
 * good, so the adapter's stderr gets its own pipe and comes back out of
 * dap_transport_next_log_line().
 *
 * Returns NULL with errno from the spawn when the child could not be
 * started, leaving no child behind; a returned transport always has a live
 * child and open descriptors. */
struct dap_transport *dap_transport_start_stdio(
    const struct kg_spawn_request *req);

/* Connect to an adapter somebody else is running: no child, no handshake,
 * one socket for both directions.  `host` is passed to framed_io, which in
 * v1 accepts only the numeric loopback address (see framed_io.h).
 *
 * The connect is nonblocking and usually still in progress when this
 * returns; it completes on a later dap_transport_flush() or
 * dap_transport_next_message(), or fails on the connect deadline.  Returns
 * NULL only when the transport could not be created at all -- a rejected
 * host or a refused connection is a live transport that has failed, so the
 * caller reads the reason from dap_transport_error() rather than errno. */
struct dap_transport *dap_transport_attach_tcp(
    const char *host, unsigned short port);

/* Wrap a socket somebody else already connected: no child, no connect, no
 * handshake, phase OPEN from the first call.  `fd` is OWNED from here on
 * and is closed by dap_transport_close() -- and on an allocation failure
 * too, so a caller never has to decide whether a NULL return kept it.
 *
 * It makes the descriptor non-blocking ITSELF rather than requiring a
 * caller to have done so, and that is the whole reason this constructor
 * says so out loud: a blocking socket here does not fail, it HANGS -- the
 * probe this design came from sat in one read() for eight minutes because
 * its caller forgot (doc/plans/dap/03-java.md, work item 1).  Every other
 * descriptor in this module arrives through framed_io, which has always
 * owned that; the one that arrives from outside must not be the exception.
 *
 * Returns NULL only on allocation failure. */
struct dap_transport *dap_transport_attach_socket(int fd);

/* Spawn `req`'s adapter, wait for it to announce the port it is listening
 * on, and connect there.  The one constructor whose work is not finished
 * when it returns: what comes back is a live transport in WAIT_ANNOUNCE,
 * and it becomes usable, or fails, on a later dap_transport_flush() or
 * dap_transport_next_message() -- the same rule dap_transport_attach_tcp()
 * already has for its connect, extended by one step in front of it.
 *
 * `announce_prefix` is the exact byte string the port follows, and it must
 * INCLUDE THE HOST: `"DAP server listening at: 127.0.0.1:"`, never
 * `"...listening at: "`.  Two reasons, both load-bearing.  A digits scan
 * after the shorter prefix reads `127` out of `127.0.0.1` and connects to
 * port 127.  And, more seriously, a client that took a host out of announce
 * text would let any line the child prints -- including anything the child
 * echoed from somewhere else -- redirect kg's connect to an arbitrary
 * address; a trusted host belongs in an explicit configuration field.  So
 * the parse is strict: the prefix, then decimal digits to the end of the
 * line, port 1-65535, connecting only to loopback.  Safe because kg is what
 * passes `--listen 127.0.0.1:0` in the first place.
 *
 * `req->stdin_fd` must be -1, as for dap_transport_start_stdio().  Returns
 * NULL with errno from the spawn when the child could not be started, and
 * with EINVAL when `announce_prefix` is missing or is not one a scanner
 * accepts. */
struct dap_transport *dap_transport_start_spawn_port(
    const struct kg_spawn_request *req, const char *announce_prefix);

/* The prefix a spawn-port transport is waiting for, or NULL on every other
 * kind.  It exists so the failure a user reads names what kg was looking
 * for -- an adapter told to log somewhere else never announces, and
 * "initialize timed out" would send them hunting in the wrong place. */
const char *dap_transport_announce_prefix(const struct dap_transport *t);

/* Whether the port announcement has not arrived yet, which is what makes a
 * failed transport an ANNOUNCE failure rather than a protocol one. */
bool dap_transport_awaiting_announce(const struct dap_transport *t);

/* Deliver the adapter's standard error as bytes rather than as log lines.
 *
 * The default is lines, because for every other adapter that channel is the
 * adapter complaining about itself.  delve is the exception measured in
 * doc/plans/dap/04-go.md: the DEBUGGEE's stdout and stderr arrive there,
 * mixed with delve's own diagnostics, and kg cannot tell them apart -- so
 * the flag is named for what it does to the channel rather than for what is
 * on it.  Once set, dap_transport_next_log_line() stops drawing from
 * standard error and dap_transport_next_stderr_bytes() is the only way to
 * read it, so nothing is delivered twice. */
void dap_transport_set_stderr_bytes(struct dap_transport *t, bool on);
int dap_transport_next_stderr_bytes(
    struct dap_transport *t, const char **data, size_t *len);

/* Connect to a listening adapter that wants a SECRET before the first
 * frame, and queue that secret ahead of everything else.
 *
 * This is the lsp-sibling wire (doc/plans/dap/03-java.md): a language
 * server announced a second server on a loopback port, and a client
 * connects there and writes the announced bytes -- no newline, no framing
 * around them -- before its first `initialize`.  The bytes go through
 * framed_io's prepend hook, so a partial write of the secret is resumed on
 * the next flush and the initialize behind it can never overtake it; a
 * caller that wrote the secret itself would have to solve both, in a
 * callback, synchronously.
 *
 * `secret` is borrowed for the call and copied; nothing here logs, formats
 * or otherwise reveals it.  The connect is nonblocking and completes on a
 * later flush or receive, exactly as dap_transport_attach_tcp()'s does, so
 * a rejected host or a refused connection is a live transport that has
 * failed rather than a NULL. */
struct dap_transport *dap_transport_connect_secret(const char *host,
    unsigned short port, const unsigned char *secret, size_t secret_len);

/* Close the protocol descriptors, make sure an owned child is gone, and
 * free everything.  NULL is accepted and ignored.
 *
 * This is the backstop under every failure path above it -- spawn, connect,
 * initialize, launch, protocol error, user cancel all end here -- so it
 * kills and reaps unconditionally rather than politely: the graceful
 * exchange is dap_transport_begin_shutdown()'s ladder and has already had
 * its chance by the time anything calls this.  On a kind that owns no child
 * it closes descriptors and signals nothing, which is the tcp-attach row of
 * the table above and the one property worth stating twice. */
void dap_transport_close(struct dap_transport *t);

/* Frame `body` (`len` bytes), queue it and try to flush.  Returns 0 when
 * the message is queued or written and -1 when the transport is dead,
 * including when it died on this call.  A return of 0 does not mean the
 * bytes have left: ask dap_transport_pending_bytes(). */
int dap_transport_send(struct dap_transport *t, const char *body, size_t len);

/* Write whatever the queue still holds and advance a connect in progress.
 * Returns 0 (including when nothing moved because the peer is not reading)
 * or -1 on a dead transport. */
int dap_transport_flush(struct dap_transport *t);

/* Pull the next complete message, reading from the adapter as needed.
 * Returns 1 with `*body`/`*len` set, 0 when no complete message has arrived
 * yet, and -1 when the transport is dead -- which is also how end of stream
 * is reported, with dap_transport_error() then being FRAMED_IO_ERR_EOF.
 * Messages buffered before the adapter died are all delivered before the
 * -1, which is what lets a session end on a disconnect response that
 * arrived in the same read as the EOF.
 *
 * `*body` points into the transport's own buffer, is NOT NUL-terminated,
 * and is valid only until the next call on this transport. */
int dap_transport_next_message(
    struct dap_transport *t, const char **body, size_t *len);

/* The next complete line the adapter logged, without its newline, or 0 when
 * there is no whole line to give.  Borrowed under
 * dap_transport_next_message()'s rule exactly.
 *
 * Two channels can feed it: the adapter's standard error (unless
 * dap_transport_set_stderr_bytes() took that one over) and, on the
 * spawn-port kind, the child's standard output once the announce has been
 * scraped off it.  Both are side channels and neither ever fails the
 * transport: they are separate pipes, so nothing on them can desynchronise
 * the framing, and a channel that ended, errored or never existed simply
 * has no more lines.  The last line before end of stream is delivered even
 * unterminated, since an adapter that died mid-sentence is the one whose
 * sentence is worth reading. */
int dap_transport_next_log_line(
    struct dap_transport *t, const char **line, size_t *len);

/* Whether the transport has failed, and why.  The codes are framed_io's own
 * rather than a parallel enum aliasing them one for one: this layer adds no
 * failure mode framed_io cannot express -- a connect that ran out of time
 * is FRAMED_IO_ERR_IO, because a connect that failed is not a state -- and a
 * second enum would be a second thing to keep in step.
 *
 * error_outbound() is which SIDE failed, because the two halves of
 * FRAMED_IO_ERR_TOO_LARGE are different events: an oversized body is the
 * adapter sending too much, an oversized outbox is kg queueing faster than
 * the adapter reads.  error_truncated() splits FRAMED_IO_ERR_PROTOCOL the
 * same way: an adapter killed mid-reply left half a frame, which is not the
 * same news as an adapter that frames wrongly. */
bool dap_transport_failed(const struct dap_transport *t);
enum framed_io_error dap_transport_error(const struct dap_transport *t);
bool dap_transport_error_outbound(const struct dap_transport *t);
bool dap_transport_error_truncated(const struct dap_transport *t);

/* Bytes queued and not yet written.  Zero is the only reliable "the request
 * is on its way", and it is what the ladder waits for before half-closing. */
size_t dap_transport_pending_bytes(const struct dap_transport *t);

enum dap_transport_kind dap_transport_kind(const struct dap_transport *t);

/* The owned child's pid, or -1 on a kind that owns none -- never 0, which
 * kg_process_signal_group() would read as kg's own process group. */
pid_t dap_transport_pid(const struct dap_transport *t);

/* Whether an owned child is still there, answered with a WNOHANG reap, so
 * asking is also how it gets collected.  False on a kind that owns no
 * child: there is nothing here to be alive. */
bool dap_transport_child_alive(struct dap_transport *t);

/* Replace the three deadlines this module enforces.  They are durations
 * from the step they belong to, so changing them mid-step changes when that
 * step ends rather than being noticed only by the next one. */
void dap_transport_set_deadlines(
    struct dap_transport *t, const struct dap_transport_deadlines *limits);

/* Start the end-of-session ladder, and advance it.  begin_shutdown() is
 * idempotent and advances one rung itself; shutdown_poll() returns 1 while
 * the ladder still has something to wait for and 0 once it is DONE or was
 * never started.  Neither blocks, and neither needs the adapter's
 * cooperation to finish. */
int dap_transport_begin_shutdown(struct dap_transport *t);
int dap_transport_shutdown_poll(struct dap_transport *t);
enum dap_transport_shutdown_state dap_transport_shutdown_state(
    const struct dap_transport *t);

/* How many descriptors dap_transport_wait_fds() may write: the protocol
 * stream, the adapter's stderr, and -- on the spawn-port kind -- the child's
 * stdout, which is the announce channel before it is a log channel. */
#define DAP_TRANSPORT_WAIT_FDS_MAX 3

/* The descriptors this transport is waiting to hear from, at most `max` of
 * them, counted by the return value.  This module polls nothing itself; it
 * says which descriptors a caller's own wait should include, so an adapter
 * is heard when it writes rather than on the editor's idle tick.
 *
 * Only descriptors the very next poll would read from are reported, which
 * is what keeps such a wait from spinning: a channel that ended, and every
 * descriptor of a failed transport, stays ready forever without yielding a
 * byte, and is left out. */
int dap_transport_wait_fds(const struct dap_transport *t, int *fds, int max);

#endif /* KG_DAP_TRANSPORT_H */
