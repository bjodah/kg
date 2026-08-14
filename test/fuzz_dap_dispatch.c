/*
 * Fuzz harness for the debug adapter's message dispatch (src/dap_client.c).
 *
 * Input encoding: the fuzz input is the raw byte stream a debug adapter
 * writes on its side of the connection, with nothing wrapped around it -- a
 * header block, a blank line, a body, as many times over as it likes.  A
 * tracked seed is therefore literally wire bytes and can be written with
 * printf(1); the ones under test/fuzz-seeds/dap_dispatch cover the three
 * message types and the ways each is malformed: responses to live and to
 * unknown request_seqs, the seq spellings (zero, string, absent,
 * out of int32 range), success and both error shapes, reverse requests with
 * and without a correlatable seq, arbitrary and unknown event bodies, and
 * the payloads the later stages parse -- breakpoint, stopped, stack,
 * scopes and variables.
 *
 * How it reaches the real client.  The bytes go into a loopback socket and
 * the client is attached to the other end with the editor's own
 * dap_transport_attach_tcp(), so what runs is the shipped
 * framed_io -> dap_transport -> dap_client stack over a real descriptor:
 * neither the framing nor the dispatch is reimplemented here.  A handful of
 * requests are registered first, so that inbound responses have live
 * request_seqs to match, mismatch and duplicate against, and the hooks are
 * a null UI -- they count and drop, which is what dap_client.h says a NULL
 * hook does and what a session-free harness wants.
 *
 * Writing and reading are interleaved rather than done in that order,
 * because a socket holds far less than an input may, and the peer is closed
 * as soon as the last byte is in: end of stream is what ends the run, so
 * every path reaches it.  The frames-level parser is fuzzed separately by
 * test/fuzz_frames.c; what is new here is everything above it.
 */
#include "../src/dap_client.h"
#include "../src/dap_transport.h"
#include "../src/json.h"
#include "../src/process.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* A bound on one input, so a mutation an order of magnitude larger than the
 * corpus costs one more socket round trip and not a timeout. */
#define MAX_FUZZ_INPUT (256u * 1024u)
/* A bound on the drive loop, for the same reason: an input that neither
 * completes a frame nor ends the stream must still terminate the run. */
#define MAX_ITERATIONS 4096
/* Requests registered before the input is played, so responses have
 * something to correlate to.  Small on purpose -- the pending table's own
 * bound is exercised by test_dap_client.c, and every extra request here is
 * bytes written before every input. */
#define PRIMED_REQUESTS 4

/* The null UI.  Every hook is reached and none of them keeps anything: what
 * the fuzzer is looking for is what the client does with the bytes on the
 * way here, not what a session would do with them afterwards. */
static volatile unsigned long sink;

static void on_event(
    void *ctx, const char *event, const struct kg_json_value *body)
{
	(void)ctx;
	sink += event ? strlen(event) : 0;
	sink += (unsigned long)kg_json_len(body);
	sink += (unsigned long)kg_json_int(kg_json_get(body, "threadId"), 0);
}

static void on_output(
    void *ctx, const char *category, const char *text, size_t len)
{
	(void)ctx;
	sink += category ? strlen(category) : 0;
	sink += text ? len : 0;
}

static void on_log(void *ctx, enum dap_log_level level, const char *text)
{
	(void)ctx;
	sink += (unsigned long)level + (text ? strlen(text) : 0);
}

static void on_response(
    struct dap_client *c, const struct dap_response *r, void *ctx)
{
	(void)c;
	(void)ctx;
	sink += (unsigned long)r->success + (unsigned long)r->answered;
	sink += r->command ? strlen(r->command) : 0;
	sink += r->error ? strlen(r->error->message) : 0;
	sink += (unsigned long)kg_json_len(r->body);
}

/* A listening loopback socket, and the port it landed on. */
static int listen_loopback(unsigned short *port)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0) {
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
	    || listen(fd, 1) != 0
	    || getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
		kg_close_fd(&fd);
		return -1;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

static bool write_would_block(void)
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

/* Push what the socket will take and answer how much of the input is in it.
 * The peer is closed once the last byte is through, and also when the
 * socket reports anything other than "not now", so the loop below always
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

/* Throw away whatever the client wrote.  A peer that never reads is one the
 * client's own writes eventually block against, which would measure the
 * socket buffer rather than the dispatcher. */
static void drain_peer(int fd)
{
	char scratch[4096];
	ssize_t n;

	while ((n = read(fd, scratch, sizeof(scratch))) > 0) {
		sink += (unsigned long)n;
	}
}

/* The questions an inbound response may be an answer to: one initialize and
 * a few ordinary requests, so request_seq 1..PRIMED_REQUESTS are live. */
static void prime_requests(struct dap_client *c)
{
	static const char *const commands[]
	    = { "threads", "stackTrace", "launch" };
	size_t i;

	(void)dap_client_initialize(c, "fuzz", on_response, NULL);
	for (i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		(void)dap_client_request(
		    c, commands[i], "{}", 2, on_response, NULL);
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct dap_client *c;
	unsigned short port = 0;
	unsigned iterations = 0;
	size_t off = 0;
	int listener;
	int peer;
	struct dap_client_hooks hooks = {
		.event = on_event,
		.output = on_output,
		.log = on_log,
	};

	if (size > MAX_FUZZ_INPUT) {
		size = MAX_FUZZ_INPUT;
	}
	listener = listen_loopback(&port);
	if (listener < 0) {
		return 0;
	}
	c = dap_client_new(dap_transport_attach_tcp("127.0.0.1", port));
	peer = c ? accept(listener, NULL, NULL) : -1;
	kg_close_fd(&listener);
	if (!c || peer < 0) {
		kg_close_fd(&peer);
		dap_client_close(c);
		return 0;
	}
	(void)fcntl(peer, F_SETFL, O_NONBLOCK);
	dap_client_set_hooks(c, &hooks);
	prime_requests(c);
	/* Terminates on either side: push_bytes() closes the peer once the
	 * input is spent, and the end of stream that follows kills the
	 * client, which is what stops the loop. */
	while (dap_client_alive(c) && iterations++ < MAX_ITERATIONS) {
		if (peer >= 0) {
			off = push_bytes(&peer, (const char *)data, size, off);
		}
		if (peer >= 0) {
			drain_peer(peer);
		}
		(void)dap_client_poll(c);
	}
	kg_close_fd(&peer);
	dap_client_close(c);
	return 0;
}
