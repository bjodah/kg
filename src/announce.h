#ifndef KG_ANNOUNCE_H
#define KG_ANNOUNCE_H

#include <stddef.h>
#include <stdint.h>

/* Pure, bounded parsing of child-process endpoint announcements.  The
 * scanner owns no descriptor and performs no connect: callers feed arbitrary
 * byte chunks and decide what a cached endpoint means. */

#define KG_ANNOUNCE_MAX_RULES 8u
#define KG_ANNOUNCE_MAX_PREFIX_BYTES 128u
#define KG_ANNOUNCE_MAX_SEPARATOR_BYTES 64u
#define KG_ANNOUNCE_MAX_LINE_BYTES 4096u
#define KG_ANNOUNCE_MAX_HOST_BYTES 64u
#define KG_ANNOUNCE_MAX_SECRET_BYTES 256u

enum kg_announce_address {
	KG_ANNOUNCE_PORT_ONLY = 0,
	KG_ANNOUNCE_HOST_PORT,
};

enum kg_announce_secret {
	KG_ANNOUNCE_SECRET_ABSENT = 0,
	KG_ANNOUNCE_SECRET_REQUIRED,
};

struct kg_announce_rule {
	unsigned tag;
	const char *prefix;
	enum kg_announce_address address;
	/* Required for PORT_ONLY and restricted to numeric loopback in v1. */
	const char *implicit_host;
	enum kg_announce_secret secret;
	/* Required exactly after the port when secret is REQUIRED. */
	const char *secret_separator;
};

struct kg_announced_endpoint {
	unsigned tag;
	char host[KG_ANNOUNCE_MAX_HOST_BYTES];
	unsigned short port;
	unsigned char secret[KG_ANNOUNCE_MAX_SECRET_BYTES];
	size_t secret_len;
	uint64_t generation;
};

enum kg_announce_line_status {
	KG_ANNOUNCE_LINE_NO_MATCH = 0,
	KG_ANNOUNCE_LINE_CACHED,
	KG_ANNOUNCE_LINE_MALFORMED,
};

struct kg_announce_scanner;

/* Rules and their strings are copied.  Tags must be nonzero and unique.
 * The only v1 host accepted, implicit or explicit, is 127.0.0.1. */
struct kg_announce_scanner *kg_announce_new(
    const struct kg_announce_rule *rules, size_t count, uint64_t generation);
void kg_announce_close(struct kg_announce_scanner *scanner);

/* Consume at most `len` bytes and report the count through `used`.  Returns
 * 1 with one completed, secret-redacted line; 0 when more bytes are needed;
 * and -1 if the line bound is exceeded.  CR immediately before LF is
 * omitted.  The returned line is borrowed until the next scanner call.
 * A completed line's parse verdict and producing rule tag are available
 * through last_status()/last_tag(), letting the owner make a primary rule
 * fatal while ignoring a malformed optional rule. */
int kg_announce_feed(struct kg_announce_scanner *scanner, const char *data,
    size_t len, size_t *used, const char **line, size_t *line_len);
int kg_announce_finish(
    struct kg_announce_scanner *scanner, const char **line, size_t *line_len);
enum kg_announce_line_status kg_announce_last_status(
    const struct kg_announce_scanner *scanner);
unsigned kg_announce_last_tag(const struct kg_announce_scanner *scanner);

/* Cached endpoints are copied out, never exposed through a pointer.  Reset
 * invalidates every endpoint and tags later matches with the new child
 * generation. */
bool kg_announce_endpoint(const struct kg_announce_scanner *scanner,
    unsigned tag, struct kg_announced_endpoint *out);
void kg_announce_reset(
    struct kg_announce_scanner *scanner, uint64_t generation);
void kg_announce_invalidate(struct kg_announce_scanner *scanner);

/* Locate the secret suffix a configured rule would redact, even when the
 * rest of the endpoint is malformed.  `tag` may be NULL; the two span
 * outputs are relative to `line`.  The bytes themselves are never copied
 * or formatted by this helper.
 *
 * A NULL scanner is a wire with no announce rules on it and answers false,
 * which is also how kg_announce_redact_line() below copies such a line out
 * whole: no rule means nothing in the line is a secret. */
bool kg_announce_secret_span(const struct kg_announce_scanner *scanner,
    const char *line, size_t len, unsigned *tag, size_t *start,
    size_t *span_len);

/* Redact a complete line without changing scanner/cache state.  Only secret
 * bytes are replaced; the logger prefix, announce prefix, address and
 * separator remain.  No helper in this module formats endpoint secrets. */
int kg_announce_redact_line(const struct kg_announce_scanner *scanner,
    const char *line, size_t len, char *out, size_t cap, size_t *out_len);

#endif /* KG_ANNOUNCE_H */
