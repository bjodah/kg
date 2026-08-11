/* Pure endpoint announce parsing, caching, generation and redaction. */

#include "../src/announce.h"
#include "test.h"

#include <string.h>

enum {
	TAG_LANGUAGE = 1,
	TAG_DEBUG = 2,
	TAG_DELVE = 3,
};

static const struct kg_announce_rule rules[] = {
	{ TAG_LANGUAGE, "Java Language Server listening at port ",
	    KG_ANNOUNCE_PORT_ONLY, "127.0.0.1", KG_ANNOUNCE_SECRET_REQUIRED,
	    " with hash " },
	{ TAG_DEBUG, "Java Debug Server Adapter listening at port ",
	    KG_ANNOUNCE_PORT_ONLY, "127.0.0.1", KG_ANNOUNCE_SECRET_REQUIRED,
	    " with hash " },
	{ TAG_DELVE, "DAP server listening at: ", KG_ANNOUNCE_HOST_PORT, NULL,
	    KG_ANNOUNCE_SECRET_ABSENT, NULL },
};

static struct kg_announce_scanner *new_scanner(uint64_t generation)
{
	struct kg_announce_scanner *scanner = kg_announce_new(
	    rules, sizeof(rules) / sizeof(rules[0]), generation);

	CHECK(scanner != NULL);
	return scanner;
}

static bool append_line(char *out, size_t cap, const char *line, size_t len)
{
	size_t used = strlen(out);

	if (used + len + 2 > cap) {
		return false;
	}
	memcpy(out + used, line, len);
	out[used + len] = '\n';
	out[used + len + 1] = '\0';
	return true;
}

static bool scan_bytes(struct kg_announce_scanner *scanner, const char *data,
    size_t len, char *out, size_t cap)
{
	const char *line;
	size_t line_len;
	size_t used;
	size_t at = 0;
	int rc;

	while (at < len) {
		rc = kg_announce_feed(
		    scanner, data + at, len - at, &used, &line, &line_len);
		if (rc < 0 || used == 0) {
			return false;
		}
		at += used;
		if (rc == 1 && !append_line(out, cap, line, line_len)) {
			return false;
		}
	}
	return true;
}

static void test_prefix_split_across_arbitrary_feeds_and_crlf(void)
{
	struct kg_announce_scanner *scanner = new_scanner(41);
	struct kg_announced_endpoint endpoint;
	char logged[512] = { 0 };

	if (!scanner) {
		return;
	}
	CHECK(scan_bytes(scanner, "noise Java Lang", strlen("noise Java Lang"),
	    logged, sizeof(logged)));
	CHECK(scan_bytes(scanner,
	    "uage Server listening at port 4567 with hash secret42\r",
	    strlen("uage Server listening at port 4567 with hash secret42\r"),
	    logged, sizeof(logged)));
	CHECK(scan_bytes(scanner, "\n", 1, logged, sizeof(logged)));
	CHECK(kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	CHECK(strcmp(endpoint.host, "127.0.0.1") == 0);
	CHECK(endpoint.port == 4567 && endpoint.generation == 41);
	CHECK(endpoint.secret_len == 8
	    && memcmp(endpoint.secret, "secret42", 8) == 0);
	CHECK(strstr(logged, "noise Java Language Server") != NULL);
	CHECK(strstr(logged, "with hash <redacted>") != NULL);
	CHECK(strstr(logged, "secret42") == NULL);
	kg_announce_close(scanner);
}

static void test_bare_and_hash_announces_in_one_read(void)
{
	static const char lines[]
	    = "Java Language Server listening at port 7000\n"
	      "Java Language Server listening at port 7000 with hash abc\n";
	struct kg_announce_scanner *scanner = new_scanner(2);
	struct kg_announced_endpoint endpoint;
	char logged[512] = { 0 };

	if (!scanner) {
		return;
	}
	CHECK(scan_bytes(
	    scanner, lines, sizeof(lines) - 1, logged, sizeof(logged)));
	CHECK(kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	CHECK(endpoint.port == 7000 && endpoint.secret_len == 3);
	CHECK(strstr(logged, "port 7000\n") != NULL);
	CHECK(strstr(logged, "abc") == NULL);
	kg_announce_close(scanner);
}

static void test_two_prefixes_cache_independently_and_redact(void)
{
	static const char lines[]
	    = "INFO Java Debug Server Adapter listening at port 8001 with hash "
	      "dsa\n"
	      "Java Language Server listening at port 8002 with hash lsp\n";
	struct kg_announce_scanner *scanner = new_scanner(9);
	struct kg_announced_endpoint debug;
	struct kg_announced_endpoint language;
	char logged[512] = { 0 };

	if (!scanner) {
		return;
	}
	CHECK(scan_bytes(
	    scanner, lines, sizeof(lines) - 1, logged, sizeof(logged)));
	CHECK(kg_announce_endpoint(scanner, TAG_DEBUG, &debug));
	CHECK(kg_announce_endpoint(scanner, TAG_LANGUAGE, &language));
	CHECK(debug.port == 8001 && language.port == 8002);
	CHECK(strstr(logged, "INFO Java Debug Server Adapter") != NULL);
	CHECK(strstr(logged, "dsa") == NULL && strstr(logged, "lsp") == NULL);
	kg_announce_close(scanner);
}

static void test_bare_loopback_endpoint_and_unterminated_final_line(void)
{
	static const char line[] = "DAP server listening at: 127.0.0.1:31337";
	struct kg_announce_scanner *scanner = new_scanner(7);
	struct kg_announced_endpoint endpoint;
	const char *rendered = NULL;
	size_t rendered_len = 0;
	size_t used = 0;

	if (!scanner) {
		return;
	}
	CHECK(kg_announce_feed(scanner, line, sizeof(line) - 1, &used,
		  &rendered, &rendered_len)
	    == 0);
	CHECK(used == sizeof(line) - 1);
	CHECK(kg_announce_finish(scanner, &rendered, &rendered_len) == 1);
	CHECK(rendered_len == sizeof(line) - 1);
	CHECK(kg_announce_endpoint(scanner, TAG_DELVE, &endpoint));
	CHECK(endpoint.port == 31337 && endpoint.secret_len == 0);
	kg_announce_close(scanner);
}

static enum kg_announce_line_status scan_one(
    struct kg_announce_scanner *scanner, const char *line)
{
	char logged[512] = { 0 };

	CHECK(scan_bytes(scanner, line, strlen(line), logged, sizeof(logged)));
	return kg_announce_last_status(scanner);
}

static void test_invalid_ports_hosts_and_hashes_are_tagged(void)
{
	struct kg_announce_scanner *scanner = new_scanner(1);
	struct kg_announced_endpoint endpoint;

	if (!scanner) {
		return;
	}
	CHECK(scan_one(scanner,
		  "Java Language Server listening at port 0 with hash abc\n")
	    == KG_ANNOUNCE_LINE_MALFORMED);
	CHECK(kg_announce_last_tag(scanner) == TAG_LANGUAGE);
	CHECK(
	    scan_one(scanner,
		"Java Language Server listening at port 65536 with hash abc\n")
	    == KG_ANNOUNCE_LINE_MALFORMED);
	CHECK(scan_one(scanner,
		  "Java Language Server listening at port 42 with hash \n")
	    == KG_ANNOUNCE_LINE_MALFORMED);
	CHECK(scan_one(scanner,
		  "Java Debug Server Adapter listening at port 9 with hash "
		  "bad\x01\n")
	    == KG_ANNOUNCE_LINE_MALFORMED);
	CHECK(kg_announce_last_tag(scanner) == TAG_DEBUG);
	CHECK(scan_one(scanner, "DAP server listening at: 0.0.0.0:1234\n")
	    == KG_ANNOUNCE_LINE_MALFORMED);
	CHECK(kg_announce_last_tag(scanner) == TAG_DELVE);
	CHECK(!kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	CHECK(!kg_announce_endpoint(scanner, TAG_DEBUG, &endpoint));
	CHECK(!kg_announce_endpoint(scanner, TAG_DELVE, &endpoint));
	kg_announce_close(scanner);
}

static void test_line_and_secret_bounds(void)
{
	static const char prefix[]
	    = "Java Language Server listening at port 1 with hash ";
	struct kg_announce_scanner *scanner = new_scanner(1);
	char line[KG_ANNOUNCE_MAX_LINE_BYTES + 2];
	const char *rendered = NULL;
	size_t rendered_len = 0;
	size_t used = 0;

	if (!scanner) {
		return;
	}
	memset(line, 'x', sizeof(line));
	CHECK(kg_announce_feed(
		  scanner, line, sizeof(line), &used, &rendered, &rendered_len)
	    == -1);
	kg_announce_reset(scanner, 2);
	memcpy(line, prefix, sizeof(prefix) - 1);
	memset(
	    line + sizeof(prefix) - 1, 'a', KG_ANNOUNCE_MAX_SECRET_BYTES + 1);
	line[sizeof(prefix) - 1 + KG_ANNOUNCE_MAX_SECRET_BYTES + 1] = '\n';
	line[sizeof(prefix) - 1 + KG_ANNOUNCE_MAX_SECRET_BYTES + 2] = '\0';
	CHECK(scan_one(scanner, line) == KG_ANNOUNCE_LINE_MALFORMED);
	kg_announce_close(scanner);
}

static void test_generation_reset_invalidates_stale_endpoint(void)
{
	struct kg_announce_scanner *scanner = new_scanner(10);
	struct kg_announced_endpoint endpoint;
	char logged[256] = { 0 };

	if (!scanner) {
		return;
	}
	CHECK(scan_bytes(scanner,
	    "Java Language Server listening at port 99 with hash old\n",
	    strlen("Java Language Server listening at port 99 with hash old\n"),
	    logged, sizeof(logged)));
	CHECK(kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	CHECK(endpoint.generation == 10);
	kg_announce_reset(scanner, 11);
	CHECK(!kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	logged[0] = '\0';
	CHECK(scan_bytes(scanner,
	    "Java Language Server listening at port 100 with hash new\n",
	    strlen(
		"Java Language Server listening at port 100 with hash new\n"),
	    logged, sizeof(logged)));
	CHECK(kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	CHECK(endpoint.generation == 11 && endpoint.port == 100);
	kg_announce_invalidate(scanner);
	CHECK(!kg_announce_endpoint(scanner, TAG_LANGUAGE, &endpoint));
	kg_announce_close(scanner);
}

int main(void)
{
	RUN(test_prefix_split_across_arbitrary_feeds_and_crlf);
	RUN(test_bare_and_hash_announces_in_one_read);
	RUN(test_two_prefixes_cache_independently_and_redact);
	RUN(test_bare_loopback_endpoint_and_unterminated_final_line);
	RUN(test_invalid_ports_hosts_and_hashes_are_tagged);
	RUN(test_line_and_secret_bounds);
	RUN(test_generation_reset_invalidates_stale_endpoint);
	return test_summary();
}
