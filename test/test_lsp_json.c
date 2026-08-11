/* test_lsp_json.c — the JSON layer under the LSP client
 * (src/json.c, Stage 2 of doc/plans/2026-08-08-lsp.md).
 *
 * Pure logic: no child process, no editor state, no transport.  Every case
 * is a byte string in and a tree or a refusal out, which is why this suite
 * runs in milliseconds and can afford to spell out the malformed inputs one
 * at a time -- and it has to, because "some JSON parser accepted it" is
 * exactly the class of bug that turns a server's typo into a client that
 * navigates somewhere wrong rather than saying no.
 *
 * The writer is checked by round trip.  Asserting the exact bytes of a
 * builder would nail down key order and spacing that no caller cares
 * about; parsing the result back and asking the tree what it holds tests
 * the property that matters, which is that a server would understand it.
 */

#include "../src/json.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

/* Parse a NUL-terminated literal.  Almost every case is spelled that way;
 * the ones with an embedded NUL pass their own length. */
static struct kg_json *parse(const char *text)
{
	return kg_json_parse(text, strlen(text), NULL);
}

/* A document that must be refused, with the offset the parser stopped at
 * left in `*off` for the cases that care where. */
static bool refuses(const char *text, size_t *off)
{
	struct kg_json *doc = kg_json_parse(text, strlen(text), off);

	if (doc) {
		kg_json_free(doc);
		return false;
	}
	return true;
}

/* Does this string node hold exactly these bytes?  Compares by length
 * first, so a case with an embedded NUL cannot pass by accident. */
static bool str_is(const struct kg_json_value *v, const char *want, size_t n)
{
	size_t len = 0;
	const char *s = kg_json_str(v, &len);

	return s && len == n && memcmp(s, want, n) == 0;
}

static void test_scalars(void)
{
	struct kg_json *doc = parse("  null  ");

	CHECK(doc != NULL);
	CHECK(kg_json_kind_of(kg_json_root(doc)) == KG_JSON_NULL);
	kg_json_free(doc);

	doc = parse("true");
	CHECK(doc != NULL);
	CHECK(kg_json_kind_of(kg_json_root(doc)) == KG_JSON_BOOL);
	CHECK(kg_json_bool(kg_json_root(doc), false) == true);
	kg_json_free(doc);

	doc = parse("false");
	CHECK(doc != NULL);
	CHECK(kg_json_bool(kg_json_root(doc), true) == false);
	kg_json_free(doc);

	doc = parse("-12.5e2");
	CHECK(doc != NULL);
	CHECK(kg_json_kind_of(kg_json_root(doc)) == KG_JSON_NUMBER);
	CHECK(kg_json_num(kg_json_root(doc), 0.0) == -1250.0);
	kg_json_free(doc);

	doc = parse("\"plain\"");
	CHECK(doc != NULL);
	CHECK(str_is(kg_json_root(doc), "plain", 5));
	kg_json_free(doc);

	doc = parse("0");
	CHECK(doc != NULL);
	CHECK(kg_json_int(kg_json_root(doc), -1) == 0);
	kg_json_free(doc);
}

/* An empty document is not a document.  It is what a server that wrote a
 * zero-length body sends, and it must not read as `null`. */
static void test_empty_input_is_refused(void)
{
	CHECK(kg_json_parse("", 0, NULL) == NULL);
	CHECK(kg_json_parse(NULL, 0, NULL) == NULL);
	CHECK(refuses("   ", NULL));
}

static void test_nested_structures(void)
{
	struct kg_json *doc = parse("{\"a\":[1,2,{\"b\":[true,null]}],"
				    "\"c\":{}}");
	const struct kg_json_value *root;
	const struct kg_json_value *inner;
	size_t key_len = 0;

	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	root = kg_json_root(doc);
	CHECK(kg_json_kind_of(root) == KG_JSON_OBJECT);
	CHECK(kg_json_len(root) == 2);
	CHECK(kg_json_len(kg_json_get(root, "a")) == 3);
	CHECK(kg_json_int(kg_json_at(kg_json_get(root, "a"), 0), -1) == 1);
	CHECK(kg_json_int(kg_json_at(kg_json_get(root, "a"), 1), -1) == 2);

	inner = kg_json_at(kg_json_get(root, "a"), 2);
	CHECK(kg_json_kind_of(inner) == KG_JSON_OBJECT);
	CHECK(kg_json_len(kg_json_get(inner, "b")) == 2);
	CHECK(kg_json_bool(kg_json_at(kg_json_get(inner, "b"), 0), false));
	CHECK(kg_json_kind_of(kg_json_at(kg_json_get(inner, "b"), 1))
	    == KG_JSON_NULL);

	/* An empty object is an object, not an absent member. */
	CHECK(kg_json_kind_of(kg_json_get(root, "c")) == KG_JSON_OBJECT);
	CHECK(kg_json_len(kg_json_get(root, "c")) == 0);

	/* Members come back in document order, named. */
	CHECK(kg_json_key_at(root, 0, &key_len) != NULL);
	CHECK(key_len == 1 && kg_json_key_at(root, 0, NULL)[0] == 'a');
	CHECK(kg_json_key_at(root, 1, NULL)[0] == 'c');
	CHECK(kg_json_key_at(root, 2, NULL) == NULL);
	/* An array's elements have no names. */
	CHECK(kg_json_key_at(kg_json_get(root, "a"), 0, NULL) == NULL);
	kg_json_free(doc);
}

/* Every accessor takes NULL, so a client chaining them writes one check at
 * the end instead of one per step.  A wrong-kind node answers like an
 * absent one. */
static void test_null_propagation(void)
{
	struct kg_json *doc = parse("{\"result\":7}");
	const struct kg_json_value *root;

	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	root = kg_json_root(doc);
	CHECK(kg_json_get(root, "missing") == NULL);
	CHECK(kg_json_get(NULL, "result") == NULL);
	CHECK(kg_json_get(root, NULL) == NULL);
	/* A number is not an object, so descending into one is not a crash. */
	CHECK(kg_json_get(kg_json_get(root, "result"), "uri") == NULL);
	CHECK(kg_json_kind_of(NULL) == KG_JSON_NONE);
	CHECK(kg_json_len(NULL) == 0);
	CHECK(kg_json_at(NULL, 0) == NULL);
	CHECK(kg_json_key_at(NULL, 0, NULL) == NULL);
	CHECK(kg_json_str(NULL, NULL) == NULL);
	CHECK(kg_json_str(kg_json_get(root, "result"), NULL) == NULL);
	CHECK(kg_json_int(NULL, 42) == 42);
	CHECK(kg_json_num(NULL, 1.5) == 1.5);
	CHECK(kg_json_bool(NULL, true) == true);
	CHECK(kg_json_root(NULL) == NULL);
	kg_json_free(NULL);

	/* Absent and null are different answers, which is what tells a
	 * "found nothing" reply from an error reply. */
	CHECK(kg_json_kind_of(kg_json_get(root, "nope")) == KG_JSON_NONE);
	kg_json_free(doc);

	doc = parse("{\"result\":null}");
	CHECK(doc != NULL);
	CHECK(kg_json_kind_of(kg_json_get(kg_json_root(doc), "result"))
	    == KG_JSON_NULL);
	kg_json_free(doc);
}

static void test_string_escapes(void)
{
	struct kg_json *doc
	    = parse("\"q:\\\" b:\\\\ s:\\/ \\b\\f\\n\\r\\t u:\\u0041\"");

	CHECK(doc != NULL);
	CHECK(str_is(kg_json_root(doc), "q:\" b:\\ s:/ \b\f\n\r\t u:A", 21));
	kg_json_free(doc);

	/* Two-byte and three-byte code points from \u, and raw UTF-8 bytes
	 * passed through untouched. */
	doc = parse("\"\\u00e5\\u4e2d\"");
	CHECK(doc != NULL);
	CHECK(str_is(kg_json_root(doc), "\xc3\xa5\xe4\xb8\xad", 5));
	kg_json_free(doc);

	doc = parse("\"r\xc3\xa5\"");
	CHECK(doc != NULL);
	CHECK(str_is(kg_json_root(doc), "r\xc3\xa5", 3));
	kg_json_free(doc);
}

/* A surrogate pair is the only way an astral character can be spelled with
 * \u, and U+1F600 is the one everybody's test file has. */
static void test_surrogate_pair(void)
{
	struct kg_json *doc = parse("\"\\ud83d\\ude00\"");

	CHECK(doc != NULL);
	CHECK(str_is(kg_json_root(doc), "\xf0\x9f\x98\x80", 4));
	kg_json_free(doc);
}

static void test_bad_strings_are_refused(void)
{
	/* A high surrogate with nothing after it, with a plain escape after
	 * it, with a non-surrogate after it, and a low surrogate alone. */
	CHECK(refuses("\"\\ud83d\"", NULL));
	CHECK(refuses("\"\\ud83d\\n\"", NULL));
	CHECK(refuses("\"\\ud83d\\u0041\"", NULL));
	CHECK(refuses("\"\\ude00\"", NULL));
	/* Escapes that are not escapes. */
	CHECK(refuses("\"\\x41\"", NULL));
	CHECK(refuses("\"\\\"", NULL));
	CHECK(refuses("\"\\u12\"", NULL));
	CHECK(refuses("\"\\u12g4\"", NULL));
	/* An unterminated string, and a raw control byte inside one: the
	 * signature of a server building JSON with printf. */
	CHECK(refuses("\"no end", NULL));
	CHECK(refuses("\"a\nb\"", NULL));
	CHECK(refuses("\"a\tb\"", NULL));
}

/* JSON's number grammar, not strtod()'s.  Leading zeros, a bare `.`, a
 * bare exponent, and the two spellings JSON explicitly does not have. */
static void test_number_grammar(void)
{
	size_t off = 0;
	struct kg_json *doc;

	CHECK(refuses("01", &off));
	CHECK(off == 1);
	CHECK(refuses("-01", NULL));
	CHECK(refuses("1.", NULL));
	CHECK(refuses(".5", NULL));
	CHECK(refuses("1e", NULL));
	CHECK(refuses("1e+", NULL));
	CHECK(refuses("+1", NULL));
	CHECK(refuses("-", NULL));
	CHECK(refuses("NaN", NULL));
	CHECK(refuses("Infinity", NULL));
	CHECK(refuses("-Infinity", NULL));
	/* A magnitude no double holds is refused rather than turned into an
	 * infinity that would travel on as a line number. */
	CHECK(refuses("1e400", NULL));
	/* 0e0 and 0.0 are legal; only `0` followed by a DIGIT is not. */
	doc = parse("0e0");
	CHECK(doc != NULL);
	CHECK(kg_json_num(kg_json_root(doc), 1.0) == 0.0);
	kg_json_free(doc);
	doc = parse("-0.5");
	CHECK(doc != NULL);
	CHECK(kg_json_num(kg_json_root(doc), 1.0) == -0.5);
	kg_json_free(doc);

	/* The rule is the INTEGER part's alone, and a parser that spread it
	 * over the whole number would refuse three spellings every server
	 * that formats with printf() emits.  Asserted on the value, not just
	 * on the verdict: `0` followed by digits that were quietly dropped
	 * would still parse. */
	doc = parse("1e007");
	CHECK(doc != NULL);
	CHECK(kg_json_num(kg_json_root(doc), 0.0) == 1e7);
	kg_json_free(doc);
	doc = parse("1.0625");
	CHECK(doc != NULL);
	CHECK(kg_json_num(kg_json_root(doc), 0.0) == 1.0625);
	kg_json_free(doc);
	doc = parse("0.0625");
	CHECK(doc != NULL);
	CHECK(kg_json_num(kg_json_root(doc), 0.0) == 0.0625);
	kg_json_free(doc);

	/* And it is enforced wherever a value may be, not only at the top:
	 * a parser that dropped it reads `[01]` as `[1]` and `{"a":01}` as
	 * `{"a":1}`, which is a wrong answer rather than a refused one --
	 * this is the mutation the rule exists to fail. */
	CHECK(refuses("[01]", &off));
	CHECK(off == 2);
	CHECK(refuses("{\"a\":01}", &off));
	CHECK(off == 6);
	CHECK(refuses("[1,-01]", NULL));
}

/* The integer accessor is a conversion, not a rounding, and it refuses
 * rather than invoking the undefined behaviour an out-of-range cast is. */
static void test_integer_accessor(void)
{
	struct kg_json *doc = parse("[2.9, -2.9, 1e30, -1e30, 42, 1e18]");
	const struct kg_json_value *a;

	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	a = kg_json_root(doc);
	CHECK(kg_json_int(kg_json_at(a, 0), -1) == 2);
	CHECK(kg_json_int(kg_json_at(a, 1), -1) == -2);
	CHECK(kg_json_int(kg_json_at(a, 2), -1) == -1);
	CHECK(kg_json_int(kg_json_at(a, 3), -1) == -1);
	CHECK(kg_json_int(kg_json_at(a, 4), -1) == 42);
	CHECK(kg_json_int(kg_json_at(a, 5), -1) == 1000000000000000000LL);
	CHECK(kg_json_at(a, 6) == NULL);
	CHECK(kg_json_int(kg_json_at(a, 6), 99) == 99);
	kg_json_free(doc);
}

/* JSON strings may hold a NUL.  The tree carries both the length and a
 * terminator, so a caller that wants strcmp() gets one and a caller that
 * needs the truth gets that too. */
static void test_embedded_nul(void)
{
	struct kg_json *doc = parse("\"a\\u0000b\"");
	const char *s;
	size_t len = 0;

	CHECK(doc != NULL);
	s = kg_json_str(kg_json_root(doc), &len);
	CHECK(s != NULL);
	CHECK(len == 3);
	CHECK(s && strlen(s) == 1);
	CHECK(s && memcmp(s, "a\0b", 4) == 0);
	kg_json_free(doc);
}

/* Bytes in, not a C string: a body with a NUL in the middle is parsed to
 * its stated length and the terminator is not a document end. */
static void test_length_is_authoritative(void)
{
	static const char body[] = "{\"a\":1}\0garbage";
	struct kg_json *doc = kg_json_parse(body, 7, NULL);

	CHECK(doc != NULL);
	CHECK(kg_json_int(kg_json_get(kg_json_root(doc), "a"), -1) == 1);
	kg_json_free(doc);
	/* The same buffer with the NUL inside the document's extent is
	 * trailing garbage, and refused. */
	CHECK(kg_json_parse(body, sizeof(body) - 1, NULL) == NULL);
}

static void test_trailing_garbage(void)
{
	size_t off = 0;

	CHECK(refuses("{} {}", &off));
	CHECK(off == 3);
	CHECK(refuses("1 2", NULL));
	CHECK(refuses("[1,2]]", NULL));
	CHECK(refuses("\"a\"x", NULL));
}

/* Every extension some JSON parser accepts, refused one at a time. */
static void test_extensions_are_refused(void)
{
	CHECK(refuses("[1,2,]", NULL));
	CHECK(refuses("{\"a\":1,}", NULL));
	CHECK(refuses("{,}", NULL));
	CHECK(refuses("[,]", NULL));
	CHECK(refuses("// c\n1", NULL));
	CHECK(refuses("[1] // c", NULL));
	CHECK(refuses("{'a':1}", NULL));
	CHECK(refuses("{a:1}", NULL));
	CHECK(refuses("{\"a\" 1}", NULL));
	CHECK(refuses("{\"a\":}", NULL));
	CHECK(refuses("{\"a\":1", NULL));
	CHECK(refuses("[1", NULL));
	CHECK(refuses("[1 2]", NULL));
	CHECK(refuses("tru", NULL));
	CHECK(refuses("nul", NULL));
	CHECK(refuses("truex", NULL));
	CHECK(refuses("{\"a\":1}}", NULL));
}

/* The depth bound is exact on both sides, because an off-by-one here is
 * either a refused legitimate reply or an unbounded recursion. */
static void test_depth_bound(void)
{
	char deep[6 * (KG_JSON_MAX_DEPTH + 1) + 8];
	struct kg_json *doc;
	unsigned n;
	size_t i;

	for (n = KG_JSON_MAX_DEPTH; n <= KG_JSON_MAX_DEPTH + 1; n++) {
		for (i = 0; i < n; i++) {
			deep[i] = '[';
			deep[2 * n - 1 - i] = ']';
		}
		deep[2 * n] = '\0';
		doc = kg_json_parse(deep, 2 * n, NULL);
		CHECKF(((n == KG_JSON_MAX_DEPTH) == (doc != NULL)),
		    "%u nested arrays: doc=%p", n, (void *)doc);
		kg_json_free(doc);
	}
	/* Objects are bounded by the same counter. */
	for (i = 0; i < KG_JSON_MAX_DEPTH + 1; i++) {
		memcpy(deep + 5 * i, "{\"a\":", 5);
	}
	memcpy(deep + 5 * (KG_JSON_MAX_DEPTH + 1), "1", 1);
	for (i = 0; i < KG_JSON_MAX_DEPTH + 1; i++) {
		deep[5 * (KG_JSON_MAX_DEPTH + 1) + 1 + i] = '}';
	}
	i = 6 * (KG_JSON_MAX_DEPTH + 1) + 1;
	CHECK(kg_json_parse(deep, i, NULL) == NULL);
}

/* An oversized document is refused before a byte of it is looked at; the
 * length is the caller's claim and this module does not have to trust it
 * enough to walk it.
 *
 * The buffer is real, and is valid JSON for every one of those bytes,
 * which is what makes this the bound's own case: a parser with the length
 * check deleted walks it, finds nothing wrong, and hands back a document.
 * Claiming that length for a two-byte `{}` -- which is how this case used
 * to be written -- proves nothing, because the trailing-garbage rule
 * refuses that one anyway, after reading 32 MiB past the end of a two-byte
 * literal, which is the very thing the bound exists to prevent.
 *
 * Nothing here costs the 32 MiB it names: the correct parser returns
 * before touching the buffer, and only a mutant pays to walk it. */
static void test_input_bound(void)
{
	const size_t len = (size_t)KG_JSON_MAX_INPUT_BYTES + 1;
	char *text = malloc(len);

	CHECK(text != NULL);
	if (!text) {
		return;
	}
	memset(text, 'a', len);
	text[0] = '"';
	text[len - 1] = '"';
	CHECK(kg_json_parse(text, len, NULL) == NULL);
	free(text);
}

/* A real clangd `textDocument/definition` reply, hand-copied.  The point is
 * not the parser -- the cases above cover that -- it is that the access
 * pattern a client writes against this shape reads the way json.h
 * promises, chained and unguarded until the end. */
static void test_clangd_definition_reply(void)
{
	static const char body[]
	    = "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":[{"
	      "\"uri\":\"file:///work/src/buffer.c\","
	      "\"range\":{\"start\":{\"line\":118,\"character\":4},"
	      "\"end\":{\"line\":118,\"character\":21}}}]}";
	struct kg_json *doc = parse(body);
	const struct kg_json_value *msg;
	const struct kg_json_value *loc;
	const struct kg_json_value *start;

	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	msg = kg_json_root(doc);
	CHECK(str_is(kg_json_get(msg, "jsonrpc"), "2.0", 3));
	CHECK(kg_json_int(kg_json_get(msg, "id"), -1) == 3);
	CHECK(kg_json_len(kg_json_get(msg, "result")) == 1);

	loc = kg_json_at(kg_json_get(msg, "result"), 0);
	CHECK(str_is(kg_json_get(loc, "uri"), "file:///work/src/buffer.c", 25));
	start = kg_json_get(kg_json_get(loc, "range"), "start");
	CHECK(kg_json_int(kg_json_get(start, "line"), -1) == 118);
	CHECK(kg_json_int(kg_json_get(start, "character"), -1) == 4);
	CHECK(kg_json_int(
		  kg_json_get(kg_json_get(kg_json_get(loc, "range"), "end"),
		      "character"),
		  -1)
	    == 21);
	/* The chain a client writes for a member no server sent. */
	CHECK(kg_json_get(kg_json_get(msg, "error"), "message") == NULL);
	kg_json_free(doc);
}

/* Build the request kg will actually send, parse it back, and ask the tree
 * what it holds.  The string deliberately carries a quote, a backslash, a
 * newline, a byte below 0x20 with no named escape, and non-ASCII UTF-8 --
 * every class the writer has to escape or pass through. */
static void test_writer_round_trip(void)
{
	static const char nasty[] = "he said \"hi\"\\path\nnext\x01 \xc3\xa5";
	struct kg_jsonw w;
	struct kg_json *doc;
	const struct kg_json_value *root;
	const struct kg_json_value *params;
	char *out = NULL;
	size_t len = 0;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "jsonrpc");
	kg_jsonw_string(&w, "2.0");
	kg_jsonw_key(&w, "id");
	kg_jsonw_int(&w, 17);
	kg_jsonw_key(&w, "method");
	kg_jsonw_string(&w, "textDocument/definition");
	kg_jsonw_key(&w, "params");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "textDocument");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "uri");
	kg_jsonw_string(&w, nasty);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "position");
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "line");
	kg_jsonw_int(&w, 0);
	kg_jsonw_key(&w, "character");
	kg_jsonw_int(&w, -3);
	kg_jsonw_end_object(&w);
	kg_jsonw_key(&w, "flags");
	kg_jsonw_begin_array(&w);
	kg_jsonw_bool(&w, true);
	kg_jsonw_bool(&w, false);
	kg_jsonw_null(&w);
	kg_jsonw_int(&w, 9007199254740993LL);
	kg_jsonw_raw(&w, "{\"pre\":[1]}", 11);
	kg_jsonw_end_array(&w);
	kg_jsonw_end_object(&w);
	kg_jsonw_end_object(&w);
	CHECK(kg_jsonw_finish(&w, &out, &len) == 0);
	CHECK(out != NULL);
	if (!out) {
		return;
	}
	CHECK(len == strlen(out));

	doc = kg_json_parse(out, len, NULL);
	CHECKF(doc != NULL, "built document did not parse: %s", out);
	if (!doc) {
		free(out);
		return;
	}
	root = kg_json_root(doc);
	CHECK(str_is(kg_json_get(root, "jsonrpc"), "2.0", 3));
	CHECK(kg_json_int(kg_json_get(root, "id"), -1) == 17);
	CHECK(
	    str_is(kg_json_get(root, "method"), "textDocument/definition", 23));
	params = kg_json_get(root, "params");
	CHECK(str_is(kg_json_get(kg_json_get(params, "textDocument"), "uri"),
	    nasty, sizeof(nasty) - 1));
	CHECK(kg_json_int(
		  kg_json_get(kg_json_get(params, "position"), "character"), 0)
	    == -3);
	CHECK(kg_json_len(kg_json_get(params, "flags")) == 5);
	CHECK(kg_json_bool(kg_json_at(kg_json_get(params, "flags"), 0), false));
	CHECK(!kg_json_bool(kg_json_at(kg_json_get(params, "flags"), 1), true));
	CHECK(kg_json_kind_of(kg_json_at(kg_json_get(params, "flags"), 2))
	    == KG_JSON_NULL);
	/* An integer beyond a double's exact range still round-trips
	 * through the writer's %lld, and comes back as the nearest double
	 * -- the reason kg_jsonw_int() is not "print a double". */
	CHECK(kg_json_num(kg_json_at(kg_json_get(params, "flags"), 3), 0.0)
	    == 9007199254740992.0);
	/* Pre-encoded bytes are spliced in as one value, structure intact. */
	CHECK(kg_json_int(
		  kg_json_at(
		      kg_json_get(
			  kg_json_at(kg_json_get(params, "flags"), 4), "pre"),
		      0),
		  -1)
	    == 1);

	/* The control byte with no named escape came back as itself, which
	 * means it went out as \u0001 and not as a raw byte. */
	CHECK(strstr(out, "\\u0001") != NULL);
	CHECK(strstr(out, "\\n") != NULL);
	CHECK(strstr(out, "\\\\") != NULL);
	/* Non-ASCII is passed through, not \u-escaped. */
	CHECK(strstr(out, "\xc3\xa5") != NULL);

	kg_json_free(doc);
	free(out);
}

/* The builder's own refusals: an unbalanced document is not handed over,
 * and neither is an empty one.  Both are the mistakes a call site makes,
 * and both are caught at the one check the header promises. */
static void test_writer_refusals(void)
{
	struct kg_jsonw w;
	char *out = (char *)"sentinel";
	size_t len = 7;

	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "a");
	kg_jsonw_int(&w, 1);
	CHECK(kg_jsonw_finish(&w, &out, &len) == -1);
	CHECK(out == NULL);
	CHECK(len == 0);

	kg_jsonw_init(&w);
	CHECK(kg_jsonw_finish(&w, &out, &len) == -1);

	/* A close with nothing open poisons the builder rather than
	 * emitting a stray bracket. */
	kg_jsonw_init(&w);
	kg_jsonw_begin_array(&w);
	kg_jsonw_end_array(&w);
	kg_jsonw_end_array(&w);
	CHECK(kg_jsonw_finish(&w, &out, &len) == -1);

	/* Abandoning a half-built document leaks nothing (valgrind is the
	 * assertion here). */
	kg_jsonw_init(&w);
	kg_jsonw_begin_object(&w);
	kg_jsonw_key(&w, "half");
	kg_jsonw_free(&w);
	/* Free after finish is harmless. */
	kg_jsonw_free(&w);
}

/* A top-level array and a top-level string are documents too, and the
 * writer emits them without an enclosing object. */
static void test_writer_top_level_array(void)
{
	struct kg_jsonw w;
	char *out = NULL;
	size_t len = 0;

	kg_jsonw_init(&w);
	kg_jsonw_begin_array(&w);
	kg_jsonw_stringn(&w, "a\0b", 3);
	kg_jsonw_end_array(&w);
	CHECK(kg_jsonw_finish(&w, &out, &len) == 0);
	if (!out) {
		return;
	}
	CHECK(strcmp(out, "[\"a\\u0000b\"]") == 0);
	free(out);
}

int main(void)
{
	RUN(test_scalars);
	RUN(test_empty_input_is_refused);
	RUN(test_nested_structures);
	RUN(test_null_propagation);
	RUN(test_string_escapes);
	RUN(test_surrogate_pair);
	RUN(test_bad_strings_are_refused);
	RUN(test_number_grammar);
	RUN(test_integer_accessor);
	RUN(test_embedded_nul);
	RUN(test_length_is_authoritative);
	RUN(test_trailing_garbage);
	RUN(test_extensions_are_refused);
	RUN(test_depth_bound);
	RUN(test_input_bound);
	RUN(test_clangd_definition_reply);
	RUN(test_writer_round_trip);
	RUN(test_writer_refusals);
	RUN(test_writer_top_level_array);
	return test_summary();
}
