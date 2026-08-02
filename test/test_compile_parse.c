/* Corpus-style tests for compile_diag_parse_line(): the pure, bounded
 * "file:line[:column]" diagnostic parser and its make Entering/Leaving
 * directory-context tracking.  No UI wiring exists yet (that is the next
 * slice's job); these tests exercise the parser and its bounded store in
 * isolation, the way the fuzz harness (test/fuzz_compile_parse.c) does. */
#include "../src/compile_parse.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

/* Feed `text` to `parser`/`store` one line at a time, split at '\n' (the
 * terminator itself is not passed to the parser, matching how compilation
 * output arrives with the newline already consumed). `text` may be a plain
 * string, never containing an embedded NUL -- tests that need one build the
 * bytes explicitly and call compile_diag_parse_line() themselves. */
static void feed_lines(struct compile_diag_parser *parser,
    struct compile_diag_store *store, const char *text)
{
	const char *p = text;

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);

		compile_diag_parse_line(parser, store, p, len);
		p += len;
		if (*p == '\n') {
			p++;
		}
	}
}

static bool diag_file_is(const struct compile_diag *d, const char *expect)
{
	size_t elen = strlen(expect);

	return d->file_len == elen && memcmp(d->file, expect, elen) == 0;
}

static bool diag_cwd_is(const struct compile_diag *d, const char *expect)
{
	size_t elen = strlen(expect);

	return d->cwd_len == elen && memcmp(d->cwd, expect, elen) == 0;
}

static void test_absolute_path_with_column(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "/work", 5);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store, "/usr/include/stdio.h:10:5: error: bad\n");

	CHECK(store.count == 1);
	CHECK(diag_file_is(&store.entries[0], "/usr/include/stdio.h"));
	CHECK(store.entries[0].line == 10);
	CHECK(store.entries[0].has_column == true);
	CHECK(store.entries[0].column == 5);
	CHECK(diag_cwd_is(&store.entries[0], "/work"));
	CHECK(store.truncated == false);
	CHECK(store.rejected_overflow == 0);
}

static void test_relative_path_no_column(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "/work/build", 11);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store, "foo.c:22: note: in expansion of macro\n");

	CHECK(store.count == 1);
	CHECK(diag_file_is(&store.entries[0], "foo.c"));
	CHECK(store.entries[0].line == 22);
	CHECK(store.entries[0].has_column == false);
	CHECK(store.entries[0].column == 0);
	CHECK(diag_cwd_is(&store.entries[0], "/work/build"));
}

static void test_path_with_spaces(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store, "my file with spaces.c:5:1: error: x\n");

	CHECK(store.count == 1);
	CHECK(diag_file_is(&store.entries[0], "my file with spaces.c"));
	CHECK(store.entries[0].line == 5);
	CHECK(store.entries[0].column == 1);
}

static void test_utf8_path(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;
	/* "café.c" -- é is 0xC3 0xA9 in UTF-8, two bytes that are never a
	 * ':' and so never confuse the colon scan. */
	const char line[] = "caf\xc3\xa9.c:7:2: error: unicode\n";

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store, line);

	CHECK(store.count == 1);
	CHECK(diag_file_is(&store.entries[0], "caf\xc3\xa9.c"));
	CHECK(store.entries[0].line == 7);
}

static void test_malformed_non_utf8_byte_in_path(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;
	/* A raw 0xFF byte is not valid UTF-8 anywhere; the parser must copy
	 * it as an ordinary byte, not interpret or reject it. */
	static const char line[] = "bad\xffname.c:8:3: error: y";

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	compile_diag_parse_line(&parser, &store, line, sizeof(line) - 1);

	CHECK(store.count == 1);
	CHECK(store.entries[0].file_len == strlen("bad\xffname.c"));
	CHECK(memcmp(store.entries[0].file, "bad\xffname.c",
		  store.entries[0].file_len)
	    == 0);
	CHECK(store.entries[0].line == 8);
	CHECK(store.entries[0].column == 3);
}

static void test_embedded_nul_in_line(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;
	/* "a\0b.c:9:1: error: z" -- an embedded NUL inside what would be a
	 * path if this were a C string.  The caller passes an explicit
	 * length, so the NUL is just another byte, not a terminator. */
	static const char line[] = "a\0b.c:9:1: error: z";

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	compile_diag_parse_line(&parser, &store, line, sizeof(line) - 1);

	CHECK(store.count == 1);
	CHECK(store.entries[0].file_len == 5);
	CHECK(memcmp(store.entries[0].file, "a\0b.c", 5) == 0);
	CHECK(store.entries[0].line == 9);
}

static void test_nested_entering_leaving_pairs(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "/root", 5);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store,
	    "make: Entering directory '/root/a'\n"
	    "make[1]: Entering directory '/root/a/b'\n"
	    "sub.c:1:1: error: inner\n"
	    "make[1]: Leaving directory '/root/a/b'\n"
	    "outer.c:2:2: error: middle\n"
	    "make: Leaving directory '/root/a'\n"
	    "top.c:3:3: error: outer\n");

	CHECK(store.count == 3);
	CHECK(diag_cwd_is(&store.entries[0], "/root/a/b"));
	CHECK(diag_cwd_is(&store.entries[1], "/root/a"));
	CHECK(diag_cwd_is(&store.entries[2], "/root"));
	CHECK(parser.unbalanced_leaving == 0);
	CHECK(parser.depth == 0);
}

static void test_backtick_quote_directory(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "/root", 5);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store,
	    "make: Entering directory `/root/old'\n"
	    "old.c:1:1: error: e\n");

	CHECK(store.count == 1);
	CHECK(diag_cwd_is(&store.entries[0], "/root/old"));
}

static void test_unbalanced_leaving(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "/base", 5);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store,
	    "make: Leaving directory '/nowhere'\n"
	    "foo.c:1:1: error: after stray leaving\n");

	CHECK(parser.unbalanced_leaving == 1);
	CHECK(store.count == 1);
	/* A stray Leaving does not change the tracked directory. */
	CHECK(diag_cwd_is(&store.entries[0], "/base"));
}

static void test_malformed_directory_line_unterminated_quote(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "/base", 5);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store, "make: Entering directory '/no/close\n");

	CHECK(parser.malformed_directory_lines == 1);
	CHECK(parser.depth == 0);
	CHECK(store.count == 0);
}

static void test_line_overflow_refused(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store,
	    "foo.c:123456789012345678901234567890:5: error: huge line\n");

	CHECK(store.count == 0);
	CHECK(store.rejected_overflow == 1);
}

static void test_column_overflow_refused(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store,
	    "foo.c:5:999999999999999999999999999999: error: huge column\n");

	CHECK(store.count == 0);
	CHECK(store.rejected_overflow == 1);
}

static void test_retention_cap_and_truncation(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;
	int i;

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	for (i = 0; i < KG_COMPILE_DIAG_MAX + 5; i++) {
		char line[64];
		int n = snprintf(line, sizeof(line), "f.c:%d:1: error: e\n", i);

		compile_diag_parse_line(&parser, &store, line, (size_t)n - 1);
	}

	CHECK(store.count == KG_COMPILE_DIAG_MAX);
	CHECK(store.truncated == true);
	/* The kept entries are the first ones seen, not the last. */
	CHECK(store.entries[0].line == 0);
}

static void test_no_diagnostics_is_not_truncated(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store, "nothing to see here\n");

	CHECK(store.count == 0);
	CHECK(store.truncated == false);
}

static void test_lines_that_look_like_diagnostics_but_are_not(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;

	compile_diag_parser_init(&parser, "", 0);
	compile_diag_store_init(&store);

	feed_lines(&parser, &store,
	    "12:34:56: not a diagnostic timestamp\n"
	    "Building target: 42 files remaining\n"
	    "note: see also\n"
	    "just a sentence with: a colon\n"
	    "1:2:3\n");

	CHECK(store.count == 0);
	CHECK(store.rejected_overflow == 0);
}

static void test_dir_stack_overflow_freezes_cwd(void)
{
	struct compile_diag_parser parser;
	struct compile_diag_store store;
	char line[128];
	int i;

	compile_diag_parser_init(&parser, "/base", 5);
	compile_diag_store_init(&store);

	for (i = 0; i < KG_COMPILE_DIAG_DIR_STACK_MAX + 4; i++) {
		int n = snprintf(line, sizeof(line),
		    "make[%d]: Entering directory '/lvl%d'\n", i, i);

		compile_diag_parse_line(&parser, &store, line, (size_t)n - 1);
	}

	CHECK(parser.stack_overflow_entries > 0);
	CHECK(parser.depth == KG_COMPILE_DIAG_DIR_STACK_MAX - 1);
}

int main(void)
{
	RUN(test_absolute_path_with_column);
	RUN(test_relative_path_no_column);
	RUN(test_path_with_spaces);
	RUN(test_utf8_path);
	RUN(test_malformed_non_utf8_byte_in_path);
	RUN(test_embedded_nul_in_line);
	RUN(test_nested_entering_leaving_pairs);
	RUN(test_backtick_quote_directory);
	RUN(test_unbalanced_leaving);
	RUN(test_malformed_directory_line_unterminated_quote);
	RUN(test_line_overflow_refused);
	RUN(test_column_overflow_refused);
	RUN(test_retention_cap_and_truncation);
	RUN(test_no_diagnostics_is_not_truncated);
	RUN(test_lines_that_look_like_diagnostics_but_are_not);
	RUN(test_dir_stack_overflow_freezes_cwd);
	return test_summary();
}
