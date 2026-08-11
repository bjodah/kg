/*
 * Fuzz harness for the LSP base-protocol frame parser (src/lsp_transport.c).
 *
 * Input encoding: the fuzz input is the raw byte stream a language server
 * writes on its standard output, with nothing wrapped around it -- a header
 * block, a blank line, a body, as many times over as it likes.  A tracked
 * seed is therefore literally wire bytes and can be written with printf(1);
 * the ones under test/fuzz-seeds/lsp_frames cover a valid response, two
 * messages in one write, a truncated header, a header block with no blank
 * line, an absurd Content-Length, a length that disagrees with the body,
 * unknown headers, and both the CRLF and the bare-LF spelling.
 *
 * How it reaches the real parser.  The bytes go into a pipe and the pipe's
 * read end is handed to lsp_transport_attach_fuzz_fd(), the KG_FUZZ seam in
 * src/lsp_transport.c, so what runs is the editor's own
 * inbox_fill()/inbox_take_message() loop over a real descriptor -- neither
 * the framing nor the read path is reimplemented here.  The other way in,
 * lsp_transport_start_wire(), forks a server per input, and a fork costs orders
 * of magnitude more than the parse it would be measuring.
 *
 * Writing and reading are interleaved rather than done in that order,
 * because a pipe holds far less than an input may, and the write end is
 * closed as soon as the last byte is in: end of stream is what ends the
 * run, so every path has to reach it.
 *
 * Every complete frame is then parsed as JSON and the tree walked, which is
 * what src/lsp_client.c's dispatch_message() does with a body and what
 * makes a sanitizer look at the bytes rather than only at the length.
 */
#include "../src/json.h"
#include "../src/lsp_transport.h"
#include "../src/process.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* A bound on one input, so that a mutation an order of magnitude larger
 * than the corpus costs one more pipe round trip and not a timeout. */
#define MAX_FUZZ_INPUT (256u * 1024u)

/* Both ends non-blocking, which is what kg_process_spawn_bidi() hands a
 * real transport: a blocking write end deadlocks the moment an input
 * outgrows the pipe, and a blocking read end hangs inbox_fill(). */
static int open_fuzz_pipe(int fds[2])
{
	if (kg_pipe_cloexec(fds) != 0) {
		return -1;
	}
	if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0
	    || fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
		kg_close_fd(&fds[0]);
		kg_close_fd(&fds[1]);
		return -1;
	}
	return 0;
}

static bool write_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

/* Push what the pipe will take and answer how much of the input is in it.
 * The write end is closed once the last byte is through, and also when the
 * pipe reports anything other than "not now", so the loop below always
 * reaches end of stream. */
static size_t push_bytes(int *fd, const char *data, size_t size, size_t off)
{
	ssize_t n;

	if (off < size) {
		n = write(*fd, data + off, size - off);
		if (n > 0) {
			off += (size_t)n;
		} else if (n == 0 || !write_would_block()) {
			off = size;
		}
	}
	if (off >= size) {
		kg_close_fd(fd);
	}
	return off;
}

/* Touch everything the JSON parser decoded: kinds, keys, string bytes,
 * numbers.  The sum is what keeps it from being dead code, and it is
 * unsigned so that wrapping is arithmetic rather than undefined.  The
 * recursion needs no depth guard of its own: the tree cannot be deeper
 * than KG_JSON_MAX_DEPTH, which the parser enforced before this ran. */
static unsigned long walk_value(const struct kg_json_value *v)
{
	unsigned long acc = (unsigned long)kg_json_kind_of(v);
	size_t count = kg_json_len(v);
	size_t len = 0;
	const char *text;
	size_t i;

	text = kg_json_str(v, &len);
	for (i = 0; text && i < len; i++) {
		acc += (unsigned char)text[i];
	}
	acc += (unsigned long)kg_json_int(v, 0);
	for (i = 0; i < count; i++) {
		if (kg_json_key_at(v, i, &len)) {
			acc += len;
		}
		acc += walk_value(kg_json_at(v, i));
	}
	return acc;
}

/* What the client does with a delivered body, minus the request
 * bookkeeping: parse it, look at it, release it. */
static void consume_body(const char *body, size_t len)
{
	/* volatile because nothing reads it: the walk exists for its
	 * dereferences, and a plain accumulator is one the compiler is
	 * entitled to delete along with them. */
	static volatile unsigned long sink;
	struct kg_json *doc = kg_json_parse(body, len, NULL);

	if (!doc) {
		return;
	}
	sink += walk_value(kg_json_root(doc));
	kg_json_free(doc);
}

/* Every message the parser can already make out of what it holds, until it
 * wants more bytes (0) or the transport is dead (-1). */
static int drain_messages(struct lsp_transport *t)
{
	const char *body = NULL;
	size_t len = 0;
	int rc;

	while ((rc = lsp_transport_next_message(t, &body, &len)) == 1) {
		consume_body(body, len);
	}
	return rc;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct lsp_transport *t;
	int fds[2];
	size_t off = 0;

	if (size > MAX_FUZZ_INPUT) {
		size = MAX_FUZZ_INPUT;
	}
	if (open_fuzz_pipe(fds) != 0) {
		return 0;
	}
	t = lsp_transport_attach_fuzz_fd(fds[0]);
	if (!t) {
		kg_close_fd(&fds[0]);
		kg_close_fd(&fds[1]);
		return 0;
	}
	/* Terminates on either side: push_bytes() closes the write end once
	 * the input is spent, and the end of stream that follows makes
	 * drain_messages() report the dead transport. */
	while (fds[1] >= 0) {
		off = push_bytes(&fds[1], (const char *)data, size, off);
		if (drain_messages(t) < 0) {
			break;
		}
	}
	kg_close_fd(&fds[1]);
	lsp_transport_close(t);
	return 0;
}
