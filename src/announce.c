/* Pure incremental parser for bounded child endpoint announcements. */

#include "announce.h"

#include <stdlib.h>
#include <string.h>

struct announce_rule_copy {
	unsigned tag;
	enum kg_announce_address address;
	enum kg_announce_secret secret;
	char prefix[KG_ANNOUNCE_MAX_PREFIX_BYTES + 1];
	char host[KG_ANNOUNCE_MAX_HOST_BYTES];
	char separator[KG_ANNOUNCE_MAX_SEPARATOR_BYTES + 1];
};

struct kg_announce_scanner {
	struct announce_rule_copy rules[KG_ANNOUNCE_MAX_RULES];
	struct kg_announced_endpoint endpoints[KG_ANNOUNCE_MAX_RULES];
	bool endpoint_valid[KG_ANNOUNCE_MAX_RULES];
	size_t rule_count;
	uint64_t generation;
	char input[KG_ANNOUNCE_MAX_LINE_BYTES + 1];
	char rendered[KG_ANNOUNCE_MAX_LINE_BYTES + 16];
	size_t input_len;
	enum kg_announce_line_status last_status;
	unsigned last_tag;
};

static size_t bytes_find(
    const char *data, size_t len, const char *needle, size_t needle_len)
{
	size_t i;

	if (needle_len > len) {
		return len;
	}
	for (i = 0; i + needle_len <= len; i++) {
		if (memcmp(data + i, needle, needle_len) == 0) {
			return i;
		}
	}
	return len;
}

/* Every string a rule carries is bounded here, whatever the arm that will
 * read it: rule_copy() copies each one it is given into a fixed field, so a
 * length checked only on the arm that uses the string is no check at all --
 * an over-long host on a HOST_PORT rule, or a leftover separator on a
 * SECRET_ABSENT rule, would still be copied. */
static bool rule_valid(const struct kg_announce_rule *rule)
{
	if (!rule->tag || !rule->prefix || rule->prefix[0] == '\0'
	    || strlen(rule->prefix) > KG_ANNOUNCE_MAX_PREFIX_BYTES) {
		return false;
	}
	if (rule->implicit_host
	    && strlen(rule->implicit_host) >= KG_ANNOUNCE_MAX_HOST_BYTES) {
		return false;
	}
	if (rule->secret_separator
	    && strlen(rule->secret_separator)
		> KG_ANNOUNCE_MAX_SEPARATOR_BYTES) {
		return false;
	}
	if (rule->address == KG_ANNOUNCE_PORT_ONLY
	    && (!rule->implicit_host
		|| strcmp(rule->implicit_host, "127.0.0.1") != 0)) {
		return false;
	}
	if (rule->address != KG_ANNOUNCE_PORT_ONLY
	    && rule->address != KG_ANNOUNCE_HOST_PORT) {
		return false;
	}
	return rule->secret == KG_ANNOUNCE_SECRET_ABSENT
	    || (rule->secret == KG_ANNOUNCE_SECRET_REQUIRED
		&& rule->secret_separator && rule->secret_separator[0] != '\0');
}

static bool tag_unique(const struct kg_announce_rule *rules, size_t at)
{
	size_t i;

	for (i = 0; i < at; i++) {
		if (rules[i].tag == rules[at].tag) {
			return false;
		}
	}
	return true;
}

static void rule_copy(
    struct announce_rule_copy *dst, const struct kg_announce_rule *src)
{
	dst->tag = src->tag;
	dst->address = src->address;
	dst->secret = src->secret;
	strcpy(dst->prefix, src->prefix);
	if (src->implicit_host) {
		strcpy(dst->host, src->implicit_host);
	}
	if (src->secret_separator) {
		strcpy(dst->separator, src->secret_separator);
	}
}

struct kg_announce_scanner *kg_announce_new(
    const struct kg_announce_rule *rules, size_t count, uint64_t generation)
{
	struct kg_announce_scanner *scanner;
	size_t i;

	if (!rules || count == 0 || count > KG_ANNOUNCE_MAX_RULES) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		if (!rule_valid(&rules[i]) || !tag_unique(rules, i)) {
			return NULL;
		}
	}
	scanner = calloc(1, sizeof(*scanner));
	if (!scanner) {
		return NULL;
	}
	for (i = 0; i < count; i++) {
		rule_copy(&scanner->rules[i], &rules[i]);
	}
	scanner->rule_count = count;
	scanner->generation = generation;
	return scanner;
}

void kg_announce_close(struct kg_announce_scanner *scanner) { free(scanner); }

static size_t parse_digits(
    const char *line, size_t len, size_t pos, unsigned long *value)
{
	for (; pos < len && line[pos] >= '0' && line[pos] <= '9'; pos++) {
		if (*value <= 65535u) {
			*value = *value * 10 + (unsigned long)(line[pos] - '0');
		}
	}
	return pos;
}

static bool secret_valid(const char *secret, size_t len)
{
	size_t i;

	if (len == 0 || len > KG_ANNOUNCE_MAX_SECRET_BYTES) {
		return false;
	}
	for (i = 0; i < len; i++) {
		if ((unsigned char)secret[i] < 0x20
		    || (unsigned char)secret[i] == 0x7f) {
			return false;
		}
	}
	return true;
}

static bool address_start(const struct announce_rule_copy *rule,
    const char *line, size_t len, size_t *pos, const char **host)
{
	static const char loopback[] = "127.0.0.1:";

	if (rule->address == KG_ANNOUNCE_PORT_ONLY) {
		*host = rule->host;
		return true;
	}
	if (len - *pos < sizeof(loopback) - 1
	    || memcmp(line + *pos, loopback, sizeof(loopback) - 1) != 0) {
		return false;
	}
	*pos += sizeof(loopback) - 1;
	*host = "127.0.0.1";
	return true;
}

enum parse_result { PARSE_IGNORE = 0, PARSE_ENDPOINT, PARSE_MALFORMED };

static enum parse_result parse_rule(const struct announce_rule_copy *rule,
    const char *line, size_t len, struct kg_announced_endpoint *endpoint)
{
	const size_t prefix_len = strlen(rule->prefix);
	const size_t sep_len = strlen(rule->separator);
	const char *host = NULL;
	unsigned long port = 0;
	size_t prefix_at;
	size_t sep_at;
	size_t start;
	size_t pos;

	prefix_at = bytes_find(line, len, rule->prefix, prefix_len);
	if (prefix_at == len) {
		return PARSE_IGNORE;
	}
	start = prefix_at + prefix_len;
	pos = start;
	if (!address_start(rule, line, len, &pos, &host)) {
		return PARSE_MALFORMED;
	}
	start = pos;
	pos = parse_digits(line, len, pos, &port);
	if (rule->secret == KG_ANNOUNCE_SECRET_REQUIRED) {
		sep_at = bytes_find(
		    line + start, len - start, rule->separator, sep_len);
		if (sep_at == len - start) {
			return PARSE_IGNORE;
		}
		sep_at += start;
		if (sep_at != pos
		    || !secret_valid(
			line + pos + sep_len, len - pos - sep_len)) {
			return PARSE_MALFORMED;
		}
	} else if (pos != len) {
		return PARSE_MALFORMED;
	}
	if (pos == start || port == 0 || port > 65535u) {
		return PARSE_MALFORMED;
	}
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->tag = rule->tag;
	strcpy(endpoint->host, host);
	endpoint->port = (unsigned short)port;
	if (rule->secret == KG_ANNOUNCE_SECRET_REQUIRED) {
		pos += sep_len;
		endpoint->secret_len = len - pos;
		memcpy(endpoint->secret, line + pos, endpoint->secret_len);
	}
	return PARSE_ENDPOINT;
}

static void scan_line(
    struct kg_announce_scanner *scanner, const char *line, size_t len)
{
	struct kg_announced_endpoint endpoint;
	enum parse_result result;
	size_t i;

	scanner->last_status = KG_ANNOUNCE_LINE_NO_MATCH;
	scanner->last_tag = 0;
	for (i = 0; i < scanner->rule_count; i++) {
		result = parse_rule(&scanner->rules[i], line, len, &endpoint);
		if (result == PARSE_MALFORMED
		    && scanner->last_status != KG_ANNOUNCE_LINE_MALFORMED) {
			scanner->last_status = KG_ANNOUNCE_LINE_MALFORMED;
			scanner->last_tag = scanner->rules[i].tag;
		} else if (result == PARSE_ENDPOINT) {
			endpoint.generation = scanner->generation;
			scanner->endpoints[i] = endpoint;
			scanner->endpoint_valid[i] = true;
			if (scanner->last_status == KG_ANNOUNCE_LINE_NO_MATCH) {
				scanner->last_status = KG_ANNOUNCE_LINE_CACHED;
				scanner->last_tag = scanner->rules[i].tag;
			}
		}
	}
}

static bool secret_span(const struct announce_rule_copy *rule, const char *line,
    size_t len, size_t *start)
{
	size_t prefix_len = strlen(rule->prefix);
	size_t sep_len = strlen(rule->separator);
	size_t prefix_at;
	size_t sep_at;

	if (rule->secret != KG_ANNOUNCE_SECRET_REQUIRED) {
		return false;
	}
	prefix_at = bytes_find(line, len, rule->prefix, prefix_len);
	if (prefix_at == len) {
		return false;
	}
	sep_at = bytes_find(line + prefix_at + prefix_len,
	    len - prefix_at - prefix_len, rule->separator, sep_len);
	if (sep_at == len - prefix_at - prefix_len) {
		return false;
	}
	*start = prefix_at + prefix_len + sep_at + sep_len;
	return true;
}

bool kg_announce_secret_span(const struct kg_announce_scanner *scanner,
    const char *line, size_t len, unsigned *tag, size_t *start,
    size_t *span_len)
{
	size_t i;

	if (!scanner) {
		return false;
	}
	for (i = 0; i < scanner->rule_count; i++) {
		if (!secret_span(&scanner->rules[i], line, len, start)) {
			continue;
		}
		if (tag) {
			*tag = scanner->rules[i].tag;
		}
		*span_len = len - *start;
		return true;
	}
	return false;
}

int kg_announce_redact_line(const struct kg_announce_scanner *scanner,
    const char *line, size_t len, char *out, size_t cap, size_t *out_len)
{
	static const char redacted[] = "<redacted>";
	size_t start;
	size_t need;
	size_t span_len;

	if (kg_announce_secret_span(
		scanner, line, len, NULL, &start, &span_len)) {
		need = start + sizeof(redacted) - 1;
		if (need > cap) {
			return -1;
		}
		memcpy(out, line, start);
		memcpy(out + start, redacted, sizeof(redacted) - 1);
		*out_len = need;
		return 1;
	}
	if (len > cap) {
		return -1;
	}
	memcpy(out, line, len);
	*out_len = len;
	return 0;
}

static int complete_line(
    struct kg_announce_scanner *scanner, const char **line, size_t *line_len)
{
	size_t len = scanner->input_len;

	if (len > 0 && scanner->input[len - 1] == '\r') {
		len--;
	}
	scan_line(scanner, scanner->input, len);
	if (kg_announce_redact_line(scanner, scanner->input, len,
		scanner->rendered, sizeof(scanner->rendered), line_len)
	    < 0) {
		return -1;
	}
	*line = scanner->rendered;
	scanner->input_len = 0;
	return 1;
}

int kg_announce_feed(struct kg_announce_scanner *scanner, const char *data,
    size_t len, size_t *used, const char **line, size_t *line_len)
{
	size_t at = 0;

	*used = 0;
	*line = NULL;
	*line_len = 0;
	while (at < len) {
		if (data[at] == '\n') {
			*used = at + 1;
			return complete_line(scanner, line, line_len);
		}
		if (scanner->input_len >= KG_ANNOUNCE_MAX_LINE_BYTES) {
			return -1;
		}
		scanner->input[scanner->input_len++] = data[at++];
	}
	*used = at;
	return 0;
}

int kg_announce_finish(
    struct kg_announce_scanner *scanner, const char **line, size_t *line_len)
{
	*line = NULL;
	*line_len = 0;
	return scanner->input_len ? complete_line(scanner, line, line_len) : 0;
}

enum kg_announce_line_status kg_announce_last_status(
    const struct kg_announce_scanner *scanner)
{
	return scanner->last_status;
}

unsigned kg_announce_last_tag(const struct kg_announce_scanner *scanner)
{
	return scanner->last_tag;
}

bool kg_announce_endpoint(const struct kg_announce_scanner *scanner,
    unsigned tag, struct kg_announced_endpoint *out)
{
	size_t i;

	if (!out) {
		return false;
	}
	for (i = 0; i < scanner->rule_count; i++) {
		if (scanner->rules[i].tag == tag && scanner->endpoint_valid[i]
		    && scanner->endpoints[i].generation
			== scanner->generation) {
			*out = scanner->endpoints[i];
			return true;
		}
	}
	return false;
}

void kg_announce_invalidate(struct kg_announce_scanner *scanner)
{
	memset(scanner->endpoint_valid, 0, sizeof(scanner->endpoint_valid));
	memset(scanner->endpoints, 0, sizeof(scanner->endpoints));
}

void kg_announce_reset(struct kg_announce_scanner *scanner, uint64_t generation)
{
	kg_announce_invalidate(scanner);
	scanner->generation = generation;
	scanner->input_len = 0;
	scanner->last_status = KG_ANNOUNCE_LINE_NO_MATCH;
	scanner->last_tag = 0;
}
