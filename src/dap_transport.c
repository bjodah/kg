/* The debug adapter's wire: a stdio child, a TCP attach or a spawned child
 * that announces its own port, child ownership, and the end-of-session
 * ladder over framed I/O. */

#include "dap_transport.h"

#include "announce.h"
#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum dap_transport_phase {
	DAP_PHASE_OPEN = 0,
	DAP_PHASE_ANNOUNCE,
	DAP_PHASE_CONNECTING,
};

/* The scanner takes a tag per rule so one channel can carry several
 * announcements; a spawn-port child has exactly one to make. */
#define DAP_TRANSPORT_ENDPOINT_ADAPTER 1u

struct dap_transport {
	enum dap_transport_kind kind;
	enum dap_transport_phase phase;
	enum dap_transport_shutdown_state shutdown;
	struct dap_transport_deadlines limits;
	struct framed_io *frames;
	/* The adapter's standard error, on its own pipe so no log line can
	 * ever land inside a frame.  Permanently absent on a kind with no
	 * child of its own. */
	struct framed_line *err;
	/* The spawn-port child's standard output: an announce channel first
	 * and a log channel afterwards, never a frame stream. */
	struct framed_line *log;
	struct kg_announce_scanner *announces;
	/* What the scan is looking for, kept so a failure can name it. */
	char announce_prefix[KG_ANNOUNCE_MAX_PREFIX_BYTES + 1];
	/* That child's standard input, which nothing here ever writes: the
	 * protocol goes over the socket.  Held so the close path can hand
	 * the child an end of input rather than leaving a pipe open. */
	int child_in_fd;
	pid_t pid;
	int64_t spawn_started_ms;
	int64_t connect_started_ms;
	int64_t step_started_ms;
	bool log_scan_done;
	bool stderr_bytes;
	bool reaped;
};

/* dap_transport.h's ownership table, as the one column code reads: whether
 * a kind's close path may signal and reap.  The other columns of that table
 * are descriptor shapes, and framed_io already decides those from the
 * descriptors themselves -- but nothing except this row can tell a
 * transport whose child it is about to kill. */
static const bool kind_owns_child[] = {
	[DAP_TRANSPORT_STDIO] = true,
	[DAP_TRANSPORT_TCP_ATTACH] = false,
	[DAP_TRANSPORT_SPAWN_PORT] = true,
	[DAP_TRANSPORT_LSP_SIBLING] = false,
};

bool dap_transport_kind_owns_child(enum dap_transport_kind kind)
{
	size_t i = (size_t)kind;

	return i < sizeof(kind_owns_child) / sizeof(kind_owns_child[0])
	    && kind_owns_child[i];
}

static int64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static struct dap_transport *transport_new(enum dap_transport_kind kind)
{
	struct dap_transport *t = calloc(1, sizeof(*t));

	if (!t) {
		return NULL;
	}
	t->kind = kind;
	t->pid = -1;
	t->child_in_fd = -1;
	t->limits.spawn_ms = DAP_TRANSPORT_SPAWN_TIMEOUT_MS;
	t->limits.connect_ms = DAP_TRANSPORT_CONNECT_TIMEOUT_MS;
	t->limits.shutdown_ms = DAP_TRANSPORT_SHUTDOWN_TIMEOUT_MS;
	t->limits.kill_ms = DAP_TRANSPORT_KILL_TIMEOUT_MS;
	t->frames = framed_io_attach_fds(-1, -1);
	t->err = framed_line_new(-1, false);
	t->log = framed_line_new(-1, false);
	if (!t->frames || !t->err || !t->log) {
		dap_transport_close(t);
		return NULL;
	}
	return t;
}

/* Take over all three descriptors of a successful spawn.  framed_io owns
 * both protocol descriptors from the adopt call on, including on its own
 * failure, so nothing here closes them twice. */
static int transport_bind_child(
    struct dap_transport *t, int in_fd, int out_fd, int err_fd)
{
	framed_line_close(t->err);
	t->err = framed_line_new(err_fd, false);
	if (!t->err) {
		kg_close_fd(&in_fd);
		kg_close_fd(&out_fd);
		return -1;
	}
	return framed_io_adopt_fds(t->frames, out_fd, in_fd);
}

struct dap_transport *dap_transport_start_stdio(
    const struct kg_spawn_request *req)
{
	struct kg_spawn_request child;
	struct dap_transport *t;
	pid_t pid = -1;
	int in_fd = -1;
	int out_fd = -1;
	int err_fd = -1;
	int saved_errno;

	if (!req) {
		errno = EINVAL;
		return NULL;
	}
	child = *req;
	child.stderr_to_output = false;
	/* The adapter, and every program it starts for us, gets no
	 * controlling terminal: a debuggee that inherited kg's would be
	 * handed the terminal's foreground process group by debugpy's
	 * launcher, and kg -- background on its own terminal, then reading
	 * EIO once the program finished -- would exit at the end of every
	 * debug run (measured; src/process.h has the mechanism). */
	child.detach_terminal = true;
	t = transport_new(DAP_TRANSPORT_STDIO);
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	if (kg_process_spawn_bidi(&child, &pid, &in_fd, &out_fd, &err_fd)
	    != 0) {
		saved_errno = errno;
		dap_transport_close(t);
		errno = saved_errno;
		return NULL;
	}
	t->pid = pid;
	if (transport_bind_child(t, in_fd, out_fd, err_fd) != 0) {
		/* The child is already running, so this failure path is one of
		 * the ones the ownership rule is about: close() reaps it. */
		saved_errno = errno;
		dap_transport_close(t);
		errno = saved_errno;
		return NULL;
	}
	return t;
}

/* ------------------------- the spawn-port kind ------------------------ */

/* The scan holds the child's stdout ahead of log delivery, exactly as the
 * language server's announce does, so a line is either wholly scanned or
 * wholly refused and nothing is delivered before its verdict is known. */
static bool spawn_port_bind_announces(
    struct dap_transport *t, const char *prefix)
{
	const struct kg_announce_rule rule
	    = { DAP_TRANSPORT_ENDPOINT_ADAPTER, prefix, KG_ANNOUNCE_PORT_ONLY,
		      "127.0.0.1", KG_ANNOUNCE_SECRET_ABSENT, NULL };

	if (strlen(prefix) > KG_ANNOUNCE_MAX_PREFIX_BYTES) {
		return false;
	}
	t->announces = kg_announce_new(&rule, 1, 1);
	if (!t->announces) {
		return false;
	}
	memcpy(t->announce_prefix, prefix, strlen(prefix) + 1);
	return true;
}

static int spawn_port_bind_child(
    struct dap_transport *t, int in_fd, int out_fd, int err_fd)
{
	framed_line_close(t->err);
	t->err = framed_line_new(err_fd, false);
	if (!t->err) {
		kg_close_fd(&in_fd);
		kg_close_fd(&out_fd);
		return -1;
	}
	framed_line_close(t->log);
	t->log = framed_line_new(out_fd, true);
	if (!t->log) {
		kg_close_fd(&in_fd);
		return -1;
	}
	t->child_in_fd = in_fd;
	t->phase = DAP_PHASE_ANNOUNCE;
	t->spawn_started_ms = monotonic_ms();
	return 0;
}

struct dap_transport *dap_transport_start_spawn_port(
    const struct kg_spawn_request *req, const char *announce_prefix)
{
	struct kg_spawn_request child;
	struct dap_transport *t;
	pid_t pid = -1;
	int in_fd = -1;
	int out_fd = -1;
	int err_fd = -1;
	int saved_errno;

	if (!req || !announce_prefix || !announce_prefix[0]) {
		errno = EINVAL;
		return NULL;
	}
	child = *req;
	child.stderr_to_output = false;
	/* Same reason as the stdio kind's: the debuggee delve starts must
	 * not inherit kg's controlling terminal. */
	child.detach_terminal = true;
	t = transport_new(DAP_TRANSPORT_SPAWN_PORT);
	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	if (!spawn_port_bind_announces(t, announce_prefix)) {
		dap_transport_close(t);
		errno = EINVAL;
		return NULL;
	}
	if (kg_process_spawn_bidi(&child, &pid, &in_fd, &out_fd, &err_fd)
	    != 0) {
		saved_errno = errno;
		dap_transport_close(t);
		errno = saved_errno;
		return NULL;
	}
	t->pid = pid;
	if (spawn_port_bind_child(t, in_fd, out_fd, err_fd) != 0) {
		saved_errno = errno;
		dap_transport_close(t);
		errno = saved_errno;
		return NULL;
	}
	return t;
}

/* The announce is the whole reason this kind exists, so a line that carries
 * the prefix and then does not parse -- a port of 0, a port past 65535, a
 * trailing word after the digits -- is a hard failure rather than a line to
 * skip.  A child whose greeting is malformed is not one to connect to on a
 * guess. */
static int announce_parse_line(
    struct dap_transport *t, const char *line, size_t len)
{
	const char *rendered = NULL;
	size_t rendered_len = 0;
	size_t used = 0;

	/* A line past the scanner's own bound is not the announce: delve's
	 * is 43 bytes and the bound is 4096.  Skipping it costs a log line
	 * its verdict, which is what it is worth. */
	if (len > KG_ANNOUNCE_MAX_LINE_BYTES) {
		return 0;
	}
	if (kg_announce_feed(
		t->announces, line, len, &used, &rendered, &rendered_len)
		!= 0
	    || used != len
	    || kg_announce_feed(
		   t->announces, "\n", 1, &used, &rendered, &rendered_len)
		!= 1) {
		return 0;
	}
	if (kg_announce_last_status(t->announces)
	    == KG_ANNOUNCE_LINE_MALFORMED) {
		return framed_io_fail(t->frames, FRAMED_IO_ERR_PROTOCOL, false);
	}
	return 0;
}

/* Connect to what the scan found, and stop scanning: from here the child's
 * stdout is an ordinary bounded log channel.  There is no secret to write
 * and nothing to redact -- the announce carries a port and nothing else. */
static int announce_connect(struct dap_transport *t)
{
	struct kg_announced_endpoint endpoint;
	int rc;

	if (!kg_announce_endpoint(
		t->announces, DAP_TRANSPORT_ENDPOINT_ADAPTER, &endpoint)) {
		return 0;
	}
	t->log_scan_done = true;
	framed_line_release_hold(t->log);
	t->connect_started_ms = monotonic_ms();
	/* Left CONNECTING even when the connect is refused outright, which a
	 * loopback port nobody listens on is: the announce DID arrive, and a
	 * failure that still claimed to be waiting for it would send a user
	 * looking for a line that is already in their log. */
	t->phase = DAP_PHASE_CONNECTING;
	rc = framed_io_connect_start(t->frames, endpoint.host, endpoint.port);
	if (rc < 0) {
		return framed_io_fail(t->frames, FRAMED_IO_ERR_IO, false);
	}
	if (rc == 1) {
		t->phase = DAP_PHASE_OPEN;
	}
	return 0;
}

static int announce_drain_lines(struct dap_transport *t)
{
	const char *line = NULL;
	size_t len = 0;

	while (framed_line_scan_next(t->log, &line, &len)) {
		if (announce_parse_line(t, line, len) < 0) {
			return -1;
		}
		if (t->phase == DAP_PHASE_ANNOUNCE) {
			if (announce_connect(t) < 0) {
				return -1;
			}
			if (t->phase != DAP_PHASE_ANNOUNCE) {
				return 0;
			}
		}
	}
	return 0;
}

/* The child's stdout ended.  Before the announce that is the startup
 * failure -- a `dlv dap` that exited, or a command that was never an
 * adapter -- and afterwards it is just a log channel closing. */
static int announce_eof(struct dap_transport *t)
{
	t->log_scan_done = true;
	framed_line_release_hold(t->log);
	if (t->phase == DAP_PHASE_ANNOUNCE) {
		return framed_io_fail(t->frames, FRAMED_IO_ERR_EOF, false);
	}
	return 0;
}

static int announce_scan(struct dap_transport *t)
{
	int rc;

	if (t->log_scan_done) {
		return 0;
	}
	for (;;) {
		if (announce_drain_lines(t) < 0) {
			return -1;
		}
		if (t->log_scan_done) {
			return 0;
		}
		if (framed_line_buffered_bytes(t->log)
		    > DAP_TRANSPORT_MAX_ANNOUNCE_BYTES) {
			framed_line_discard_buffer(t->log);
			return framed_io_fail(
			    t->frames, FRAMED_IO_ERR_TOO_LARGE, false);
		}
		rc = framed_line_fill(t->log);
		if (rc == 0) {
			return 0;
		}
		if (rc < 0) {
			return announce_drain_lines(t) < 0 ? -1
							   : announce_eof(t);
		}
	}
}

/* The deadline for a child that will never announce: it never wrote the
 * line, and nothing about its descriptors says so -- an adapter told to log
 * to a file keeps its stdout open and silent forever. */
static int announce_advance(struct dap_transport *t)
{
	if (announce_scan(t) < 0) {
		return -1;
	}
	if (t->phase != DAP_PHASE_ANNOUNCE) {
		return 0;
	}
	if (monotonic_ms() - t->spawn_started_ms
	    >= (int64_t)t->limits.spawn_ms) {
		return framed_io_fail(t->frames, FRAMED_IO_ERR_IO, false);
	}
	return 0;
}

const char *dap_transport_announce_prefix(const struct dap_transport *t)
{
	return t->announce_prefix[0] ? t->announce_prefix : NULL;
}

bool dap_transport_awaiting_announce(const struct dap_transport *t)
{
	return t->phase == DAP_PHASE_ANNOUNCE;
}

void dap_transport_set_stderr_bytes(struct dap_transport *t, bool on)
{
	t->stderr_bytes = on;
}

int dap_transport_next_stderr_bytes(
    struct dap_transport *t, const char **data, size_t *len)
{
	*data = NULL;
	*len = 0;
	if (!t->stderr_bytes) {
		return 0;
	}
	/* The other channel may hold the outstanding borrow; end it before
	 * this one supplies the next. */
	framed_line_discard_borrow(t->log);
	return framed_line_next_bytes(t->err, data, len);
}

struct dap_transport *dap_transport_attach_tcp(
    const char *host, unsigned short port)
{
	struct dap_transport *t = transport_new(DAP_TRANSPORT_TCP_ATTACH);
	int rc;

	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	t->connect_started_ms = monotonic_ms();
	rc = framed_io_connect_start(t->frames, host, port);
	if (rc < 0) {
		/* A rejected host or port leaves framed_io healthy and says so
		 * through errno only.  The transport is not healthy, and a
		 * caller must not have to read one failure two ways. */
		(void)framed_io_fail(t->frames, FRAMED_IO_ERR_IO, false);
		return t;
	}
	t->phase = rc == 1 ? DAP_PHASE_OPEN : DAP_PHASE_CONNECTING;
	return t;
}

struct dap_transport *dap_transport_attach_socket(int fd)
{
	struct dap_transport *t = transport_new(DAP_TRANSPORT_LSP_SIBLING);

	if (!t) {
		kg_close_fd(&fd);
		errno = ENOMEM;
		return NULL;
	}
	/* framed_io owns the descriptor from here, including on its own
	 * failure, and it is what makes it O_NONBLOCK and FD_CLOEXEC -- the
	 * guarantee dap_transport.h states, kept in the one place that has
	 * always kept it for every other descriptor in this module. */
	(void)framed_io_adopt_fds(t->frames, fd, fd);
	/* No child to reap, ever: the language server owns the process this
	 * socket belongs to, and a debug session ending must leave it
	 * running.  `reaped` says so, so no close path can even ask. */
	t->reaped = true;
	return t;
}

struct dap_transport *dap_transport_connect_secret(const char *host,
    unsigned short port, const unsigned char *secret, size_t secret_len)
{
	struct dap_transport *t = transport_new(DAP_TRANSPORT_LSP_SIBLING);
	int rc;

	if (!t) {
		errno = ENOMEM;
		return NULL;
	}
	t->reaped = true;
	t->connect_started_ms = monotonic_ms();
	rc = framed_io_connect_start(t->frames, host, port);
	if (rc < 0) {
		(void)framed_io_fail(t->frames, FRAMED_IO_ERR_IO, false);
		return t;
	}
	t->phase = rc == 1 ? DAP_PHASE_OPEN : DAP_PHASE_CONNECTING;
	/* Queued BEFORE any caller can send: prepend puts it at the head of
	 * the outbox, and everything framed afterwards is behind it whether
	 * or not the connect has completed. */
	if (secret_len > 0
	    && framed_io_prepend(t->frames, (const char *)secret, secret_len)
		!= 0) {
		return t;
	}
	return t;
}

void dap_transport_close(struct dap_transport *t)
{
	struct kg_process_status status;

	if (!t) {
		return;
	}
	framed_io_close(t->frames);
	framed_line_close(t->err);
	framed_line_close(t->log);
	kg_announce_close(t->announces);
	kg_close_fd(&t->child_in_fd);
	if (!t->reaped && t->pid > 0
	    && dap_transport_kind_owns_child(t->kind)) {
		kg_process_signal_group(t->pid, SIGKILL);
		kg_process_wait(t->pid, &status);
		t->reaped = true;
	}
	free(t);
}

/* The connect deadline is enforced here and is the one deadline no native
 * case reaches the expiry of: framed_io accepts only the loopback address
 * in v1, and a loopback connect completes or is refused inside the kernel
 * rather than hanging, so nothing deterministic can hold one open long
 * enough.  It is enforced anyway -- the moment framed_io reaches a host
 * that can drop a packet, a wedged connect is a session with no readable
 * descriptor to notice it by -- and the suite covers the two edges that do
 * exist here, a connect that completes and an argument that is refused. */
static int connect_advance(struct dap_transport *t)
{
	int rc = framed_io_connect_finish(t->frames);

	if (rc != 0) {
		if (rc > 0) {
			t->phase = DAP_PHASE_OPEN;
		}
		return rc;
	}
	if (monotonic_ms() - t->connect_started_ms
	    >= (int64_t)t->limits.connect_ms) {
		return framed_io_fail(t->frames, FRAMED_IO_ERR_IO, false);
	}
	return 0;
}

/* 1 when the protocol stream is usable, 0 while the announce or the connect
 * is still in progress, -1 once the transport is dead. */
static int transport_advance(struct dap_transport *t)
{
	if (framed_io_failed(t->frames)) {
		return -1;
	}
	if (t->kind == DAP_TRANSPORT_SPAWN_PORT && !t->log_scan_done
	    && announce_advance(t) < 0) {
		return -1;
	}
	if (t->phase == DAP_PHASE_ANNOUNCE) {
		return 0;
	}
	if (t->phase == DAP_PHASE_CONNECTING) {
		return connect_advance(t);
	}
	return 1;
}

int dap_transport_flush(struct dap_transport *t)
{
	int rc = transport_advance(t);

	if (rc <= 0) {
		return rc;
	}
	return framed_io_flush(t->frames);
}

int dap_transport_send(struct dap_transport *t, const char *body, size_t len)
{
	if (framed_io_send(t->frames, body, len) != 0) {
		return -1;
	}
	return dap_transport_flush(t);
}

int dap_transport_next_message(
    struct dap_transport *t, const char **body, size_t *len)
{
	int rc = transport_advance(t);

	if (rc <= 0) {
		*body = NULL;
		*len = 0;
		return rc;
	}
	return framed_io_next_message(t->frames, body, len);
}

int dap_transport_next_log_line(
    struct dap_transport *t, const char **line, size_t *len)
{
	*line = NULL;
	*len = 0;
	/* Keep the announce scan ahead of what this call may expose: log
	 * draining stays useful after the protocol stream has failed, and on
	 * the spawn-port kind the same descriptor is both channels. */
	if (t->kind == DAP_TRANSPORT_SPAWN_PORT && !t->log_scan_done) {
		(void)announce_advance(t);
	}
	/* Either channel may have supplied the last borrow; end both before
	 * choosing which one supplies this call. */
	framed_line_discard_borrow(t->err);
	framed_line_discard_borrow(t->log);
	if (!t->stderr_bytes && framed_line_next(t->err, line, len)) {
		return 1;
	}
	return framed_line_next(t->log, line, len);
}

bool dap_transport_failed(const struct dap_transport *t)
{
	return framed_io_failed(t->frames);
}

enum framed_io_error dap_transport_error(const struct dap_transport *t)
{
	return framed_io_error(t->frames);
}

bool dap_transport_error_outbound(const struct dap_transport *t)
{
	return framed_io_error_outbound(t->frames);
}

bool dap_transport_error_truncated(const struct dap_transport *t)
{
	return framed_io_error_truncated(t->frames);
}

size_t dap_transport_pending_bytes(const struct dap_transport *t)
{
	return framed_io_pending_bytes(t->frames);
}

enum dap_transport_kind dap_transport_kind(const struct dap_transport *t)
{
	return t->kind;
}

pid_t dap_transport_pid(const struct dap_transport *t) { return t->pid; }

bool dap_transport_child_alive(struct dap_transport *t)
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

void dap_transport_set_deadlines(
    struct dap_transport *t, const struct dap_transport_deadlines *limits)
{
	if (limits) {
		t->limits = *limits;
	}
}

static unsigned step_limit_ms(const struct dap_transport *t)
{
	switch (t->shutdown) {
	case DAP_TRANSPORT_SHUTDOWN_FLUSHING:
	case DAP_TRANSPORT_SHUTDOWN_GRACE:
		return t->limits.shutdown_ms;
	case DAP_TRANSPORT_SHUTDOWN_SIGNALLED:
	case DAP_TRANSPORT_SHUTDOWN_KILLED:
		return t->limits.kill_ms;
	default:
		return 0;
	}
}

static bool step_expired(const struct dap_transport *t)
{
	return monotonic_ms() - t->step_started_ms >= (int64_t)step_limit_ms(t);
}

static void shutdown_enter(
    struct dap_transport *t, enum dap_transport_shutdown_state state)
{
	if (state == DAP_TRANSPORT_SHUTDOWN_GRACE) {
		/* The spawn-port child's stdin is the one descriptor the
		 * framing layer does not own, so its end of input is this
		 * ladder's to give at the same rung as the half-close. */
		kg_close_fd(&t->child_in_fd);
	}
	t->shutdown = state;
	t->step_started_ms = monotonic_ms();
}

/* The outbox drains before the half-close, because the message that ends a
 * session is usually the one still queued. */
static void shutdown_flush(struct dap_transport *t)
{
	if (framed_io_failed(t->frames)) {
		/* The protocol descriptors are already closed, which is more
		 * than a half-close; the adapter's grace starts now. */
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_GRACE);
		return;
	}
	(void)framed_io_flush(t->frames);
	if (framed_io_pending_bytes(t->frames) == 0) {
		(void)framed_io_half_close_output(t->frames);
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_GRACE);
		return;
	}
	if (step_expired(t)) {
		/* An adapter that stopped reading never received the request
		 * that would have ended this politely.  That is kg queueing
		 * faster than it reads, so it is recorded outbound. */
		(void)framed_io_fail(t->frames, FRAMED_IO_ERR_IO, true);
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_GRACE);
	}
}

/* An owned child that has not gone by the grace deadline gets SIGTERM.  A
 * kind that owns none is never alive here, so it leaves for DONE on the
 * first poll and no signal is ever sent on its behalf. */
static void shutdown_grace(struct dap_transport *t)
{
	if (!dap_transport_child_alive(t)) {
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_DONE);
		return;
	}
	if (step_expired(t)) {
		kg_process_signal_group(t->pid, SIGTERM);
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_SIGNALLED);
	}
}

static void shutdown_signalled(struct dap_transport *t)
{
	if (!dap_transport_child_alive(t)) {
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_DONE);
		return;
	}
	if (step_expired(t)) {
		kg_process_signal_group(t->pid, SIGKILL);
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_KILLED);
	}
}

/* Nothing escalates past SIGKILL: the ladder waits for the reap, and
 * dap_transport_close()'s blocking wait is the only rung below this one. */
static void shutdown_killed(struct dap_transport *t)
{
	if (!dap_transport_child_alive(t)) {
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_DONE);
	}
}

int dap_transport_shutdown_poll(struct dap_transport *t)
{
	switch (t->shutdown) {
	case DAP_TRANSPORT_SHUTDOWN_FLUSHING:
		shutdown_flush(t);
		break;
	case DAP_TRANSPORT_SHUTDOWN_GRACE:
		shutdown_grace(t);
		break;
	case DAP_TRANSPORT_SHUTDOWN_SIGNALLED:
		shutdown_signalled(t);
		break;
	case DAP_TRANSPORT_SHUTDOWN_KILLED:
		shutdown_killed(t);
		break;
	case DAP_TRANSPORT_SHUTDOWN_IDLE:
	case DAP_TRANSPORT_SHUTDOWN_DONE:
		break;
	}
	return t->shutdown != DAP_TRANSPORT_SHUTDOWN_IDLE
	    && t->shutdown != DAP_TRANSPORT_SHUTDOWN_DONE;
}

int dap_transport_begin_shutdown(struct dap_transport *t)
{
	if (t->shutdown == DAP_TRANSPORT_SHUTDOWN_IDLE) {
		shutdown_enter(t, DAP_TRANSPORT_SHUTDOWN_FLUSHING);
	}
	return dap_transport_shutdown_poll(t);
}

enum dap_transport_shutdown_state dap_transport_shutdown_state(
    const struct dap_transport *t)
{
	return t->shutdown;
}

static void wait_fd_add(int fd, int *fds, int max, int *count)
{
	if (fd >= 0 && *count < max) {
		fds[(*count)++] = fd;
	}
}

int dap_transport_wait_fds(const struct dap_transport *t, int *fds, int max)
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
