/* LSP child, nbcode announce/connect policy, and framed I/O composition. */

#include "lsp_transport.h"

#include "process.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
	struct framed_io *frames;
	struct framed_line *err;
	struct framed_line *log;
	struct kg_announce_scanner *announces;
	char redacted[FRAMED_IO_MAX_LINE_BYTES + 16];
	uint64_t generation;
	bool log_scan_done;
	bool reaped;
};

static uint64_t next_child_generation = 1;

static void transport_release_lines(struct lsp_transport *t)
{
	framed_line_release_hold(t->err);
	/* A listen/hash stdout remains a secret-bearing announce channel even
	 * after its protocol socket fails.  Keep its physical-line scan ahead
	 * of bounded log delivery until stdout itself ends; otherwise a late
	 * sibling announce could cross a delivery cut and evade redaction. */
	if (t->wire != LSP_WIRE_LISTEN_HASH || t->log_scan_done) {
		framed_line_release_hold(t->log);
	}
}

static void transport_invalidate_announces(struct lsp_transport *t)
{
	if (t->announces) {
		kg_announce_invalidate(t->announces);
	}
}

static int transport_fail(
    struct lsp_transport *t, enum lsp_transport_error error, bool outbound)
{
	transport_release_lines(t);
	kg_close_fd(&t->child_in_fd);
	return framed_io_fail(t->frames, (enum framed_io_error)error, outbound);
}

/* A framing call has already recorded the sticky reason.  Apply the outer
 * descriptor policy too; listen/hash deliberately retains its log hold so
 * physical-line secret scanning can continue while the child is alive. */
static int transport_frame_failed(struct lsp_transport *t)
{
	transport_release_lines(t);
	kg_close_fd(&t->child_in_fd);
	return -1;
}

static int connect_finish(struct lsp_transport *t)
{
	int rc = framed_io_connect_finish(t->frames);

	if (rc <= 0) {
		if (rc == 0) {
			return 0;
		}
		return transport_frame_failed(t);
	}
	t->phase = LSP_PHASE_OPEN;
	return 1;
}

static int announce_connect_language(struct lsp_transport *t)
{
	struct kg_announced_endpoint endpoint;
	int rc;

	if (t->phase != LSP_PHASE_ANNOUNCE
	    || !kg_announce_endpoint(
		t->announces, LSP_TRANSPORT_ENDPOINT_LANGUAGE, &endpoint)) {
		return 0;
	}
	if (framed_io_prepend(
		t->frames, (const char *)endpoint.secret, endpoint.secret_len)
	    != 0) {
		return transport_frame_failed(t);
	}
	rc = framed_io_connect_start(t->frames, endpoint.host, endpoint.port);
	if (rc < 0) {
		return transport_frame_failed(t);
	}
	t->phase = rc == 1 ? LSP_PHASE_OPEN : LSP_PHASE_CONNECTING;
	return 1;
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

static size_t announce_candidate(const char *line, size_t len, unsigned *tag)
{
	size_t language = line_find(line, len, LSP_TRANSPORT_ANNOUNCE_PREFIX,
	    sizeof(LSP_TRANSPORT_ANNOUNCE_PREFIX) - 1);
	size_t debug = line_find(line, len, LSP_TRANSPORT_DEBUG_ANNOUNCE_PREFIX,
	    sizeof(LSP_TRANSPORT_DEBUG_ANNOUNCE_PREFIX) - 1);

	if (language <= debug && language < len) {
		*tag = LSP_TRANSPORT_ENDPOINT_LANGUAGE;
		return language;
	}
	if (debug < len) {
		*tag = LSP_TRANSPORT_ENDPOINT_JAVA_DEBUG;
		return debug;
	}
	return len;
}

static int announce_redact_scanned(
    struct lsp_transport *t, const char *line, size_t len)
{
	static const char replacement[] = "<redacted>";
	size_t secret_at;
	size_t secret_len;

	if (!kg_announce_secret_span(
		t->announces, line, len, NULL, &secret_at, &secret_len)) {
		return 0;
	}
	if (!framed_line_replace_scanned_span(t->log, secret_at, secret_len,
		replacement, sizeof(replacement) - 1)) {
		return transport_fail(t, LSP_TRANSPORT_ERR_NOMEM, false);
	}
	return 0;
}

/* Hand one physical line and its terminator to the bounded scanner, so it
 * is either wholly scanned or wholly refused.  The rendered line is
 * discarded: this path exists to update the scanner's verdict and cache,
 * and what the user reads is the redaction applied to the log channel's own
 * copy.  Returns 0, or -1 when the scanner would not take every byte. */
static int announce_feed_line(
    struct lsp_transport *t, const char *at, size_t len)
{
	const char *rendered = NULL;
	size_t rendered_len = 0;
	size_t used = 0;

	if (kg_announce_feed(
		t->announces, at, len, &used, &rendered, &rendered_len)
		!= 0
	    || used != len) {
		return -1;
	}
	if (kg_announce_feed(
		t->announces, "\n", 1, &used, &rendered, &rendered_len)
		!= 1
	    || used != 1) {
		return -1;
	}
	return 0;
}

static int announce_parse_line(
    struct lsp_transport *t, const char *line, size_t len)
{
	enum kg_announce_line_status status;
	size_t candidate_at;
	unsigned candidate_tag = 0;
	unsigned status_tag;
	int rc;

	candidate_at = announce_candidate(line, len, &candidate_tag);
	if (candidate_at == len) {
		return 0;
	}
	if (len - candidate_at > KG_ANNOUNCE_MAX_LINE_BYTES) {
		rc = announce_redact_scanned(t, line, len);
		if (rc < 0
		    || candidate_tag == LSP_TRANSPORT_ENDPOINT_JAVA_DEBUG) {
			return rc;
		}
		return transport_fail(t, LSP_TRANSPORT_ERR_TOO_LARGE, false);
	}
	/* Redaction comes before the failure on every exit from here, not
	 * only the ones the transport survives: the scanner has already run
	 * past the secret, and the log channel's copy of this line is
	 * deliverable to *lsp-log* whether or not the transport lives. */
	if (announce_feed_line(t, line + candidate_at, len - candidate_at)
	    < 0) {
		if (announce_redact_scanned(t, line, len) < 0) {
			return -1;
		}
		return transport_fail(t, LSP_TRANSPORT_ERR_TOO_LARGE, false);
	}
	status = kg_announce_last_status(t->announces);
	status_tag = kg_announce_last_tag(t->announces);
	if (announce_redact_scanned(t, line, len) < 0) {
		return -1;
	}
	if (status == KG_ANNOUNCE_LINE_MALFORMED
	    && status_tag == LSP_TRANSPORT_ENDPOINT_LANGUAGE) {
		return transport_fail(t, LSP_TRANSPORT_ERR_PROTOCOL, false);
	}
	return announce_connect_language(t);
}

static int announce_drain_lines(struct lsp_transport *t)
{
	const char *line = NULL;
	size_t len = 0;
	int rc;

	while (framed_line_scan_next(t->log, &line, &len)) {
		rc = announce_parse_line(t, line, len);
		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static int announce_eof(struct lsp_transport *t)
{
	if (t->phase == LSP_PHASE_ANNOUNCE) {
		return transport_fail(t, LSP_TRANSPORT_ERR_EOF, false);
	}
	t->log_scan_done = true;
	framed_line_release_hold(t->log);
	return 0;
}

static int announce_scan(struct lsp_transport *t)
{
	int rc;

	if (t->log_scan_done) {
		return 0;
	}
	for (;;) {
		rc = announce_drain_lines(t);
		if (rc < 0) {
			return rc;
		}
		if (framed_line_buffered_bytes(t->log)
		    > LSP_TRANSPORT_MAX_ANNOUNCE_BYTES) {
			framed_line_discard_buffer(t->log);
			return transport_fail(
			    t, LSP_TRANSPORT_ERR_TOO_LARGE, false);
		}
		rc = framed_line_fill(t->log);
		if (rc <= 0) {
			if (rc == 0) {
				return 0;
			}
			rc = announce_drain_lines(t);
			return rc < 0 ? rc : announce_eof(t);
		}
	}
}

static int transport_scan_log(struct lsp_transport *t)
{
	return t->wire == LSP_WIRE_LISTEN_HASH ? announce_scan(t) : 0;
}

static int transport_advance(struct lsp_transport *t)
{
	int rc;

	if (framed_io_failed(t->frames)) {
		return -1;
	}
	rc = transport_scan_log(t);
	if (rc < 0) {
		return rc;
	}
	if (t->phase == LSP_PHASE_ANNOUNCE) {
		return 0;
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
	return t;
}

static const struct kg_announce_rule lsp_announce_rules[] = {
	{ LSP_TRANSPORT_ENDPOINT_LANGUAGE, LSP_TRANSPORT_ANNOUNCE_PREFIX,
	    KG_ANNOUNCE_PORT_ONLY, "127.0.0.1", KG_ANNOUNCE_SECRET_REQUIRED,
	    LSP_TRANSPORT_ANNOUNCE_HASH },
	{ LSP_TRANSPORT_ENDPOINT_JAVA_DEBUG,
	    LSP_TRANSPORT_DEBUG_ANNOUNCE_PREFIX, KG_ANNOUNCE_PORT_ONLY,
	    "127.0.0.1", KG_ANNOUNCE_SECRET_REQUIRED,
	    LSP_TRANSPORT_ANNOUNCE_HASH },
};

static int transport_create_parts(struct lsp_transport *t)
{
	t->frames = framed_io_new(-1, -1);
	t->err = framed_line_new(-1, false);
	t->log = framed_line_new(-1, false);
	return t->frames && t->err && t->log ? 0 : -1;
}

static struct lsp_transport *transport_new(void)
{
	struct lsp_transport *t = transport_alloc();

	if (!t) {
		return NULL;
	}
	if (transport_create_parts(t) != 0) {
		framed_io_close(t->frames);
		framed_line_close(t->err);
		framed_line_close(t->log);
		kg_announce_close(t->announces);
		free(t);
		return NULL;
	}
	return t;
}

static void transport_set_generation(struct lsp_transport *t)
{
	t->generation = next_child_generation++;
	if (next_child_generation == 0) {
		next_child_generation = 1;
	}
}

static bool transport_create_announces(struct lsp_transport *t)
{
	t->announces = kg_announce_new(lsp_announce_rules,
	    sizeof(lsp_announce_rules) / sizeof(lsp_announce_rules[0]),
	    t->generation);
	return t->announces != NULL;
}

static int transport_bind_listen_child(
    struct lsp_transport *t, int in_fd, int out_fd)
{
	if (!transport_create_announces(t)) {
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
	t->phase = LSP_PHASE_ANNOUNCE;
	return 0;
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
		return transport_bind_listen_child(t, in_fd, out_fd);
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
	transport_set_generation(t);
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
	kg_announce_close(t->announces);
	kg_close_fd(&t->child_in_fd);
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

static int transport_next_log_line(
    struct lsp_transport *t, const char **line, size_t *len)
{
	const char *raw = NULL;
	size_t raw_len = 0;

	if (!framed_line_next(t->log, &raw, &raw_len)) {
		return 0;
	}
	if (kg_announce_redact_line(t->announces, raw, raw_len, t->redacted,
		sizeof(t->redacted), len)
	    < 0) {
		return 0;
	}
	*line = t->redacted;
	return 1;
}

int lsp_transport_next_stderr_line(
    struct lsp_transport *t, const char **line, size_t *len)
{
	*line = NULL;
	*len = 0;
	/* Log draining remains useful after a frame failure.  For listen/hash
	 * it must also keep the physical-line scanner ahead of the bytes it
	 * exposes, so later sibling secrets are redacted under the same rule.
	 */
	(void)transport_scan_log(t);
	/* Either channel may have supplied the last borrow; invalidate both
	 * before choosing which one supplies this call. */
	framed_line_discard_borrow(t->err);
	framed_line_discard_borrow(t->log);
	if (framed_line_next(t->err, line, len)) {
		return 1;
	}
	return transport_next_log_line(t, line, len);
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

bool lsp_transport_error_truncated(const struct lsp_transport *t)
{
	return framed_io_error_truncated(t->frames);
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
		transport_invalidate_announces(t);
		return false;
	}
	return true;
}

bool lsp_transport_announced_endpoint(struct lsp_transport *t,
    enum lsp_transport_endpoint_tag tag, struct kg_announced_endpoint *endpoint)
{
	if (!t || !endpoint || !t->announces || !lsp_transport_child_alive(t)) {
		return false;
	}
	return kg_announce_endpoint(t->announces, (unsigned)tag, endpoint);
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
