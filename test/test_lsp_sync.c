#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE /* mkdtemp, realpath under -std=c2x */
#endif

/* test_lsp_sync.c — positions, file URIs and document synchronisation
 * (src/lsp_sync.c, src/lsp_uri.c; Stage 4 of doc/plans/2026-08-08-lsp.md).
 *
 * Three layers, and each is tested at the level it can actually be wrong.
 *
 * The position and URI helpers are pure functions over bytes, so they are
 * driven with string literals: multibyte fixtures on both encodings, and
 * every URI kg must refuse, because a URI accepted by mistake is a command
 * that opens the wrong file.
 *
 * The synchronisation is a conversation, so it drives a real child --
 * test/fake_lsp_server.py in `protocol` mode with `--record`, which appends
 * every textDocument/did* notification it receives to a file as JSON.  The
 * assertions are therefore against the exact payload kg sent, not against
 * an effect of it: a didChange with the range off by one glyph is a defect
 * no amount of "the server answered" would reveal.
 *
 * Nothing sleeps blind and nothing sleeps to prove an absence.  Every case
 * ends with a barrier -- a kg/echo request pumped to its answer -- and the
 * fake handles messages in order, so once the echo has been answered every
 * notification kg sent before it is already in the record file.  "No
 * didChange was sent" is then a fact rather than a timeout.
 */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "../src/json.h"
#include "../src/lsp_client.h"
#include "../src/lsp_sync.h"
#include "../src/lsp_uri.h"
#include "../src/process.h"
#include "../src/syntax.h"
#include "test.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Long enough that a loaded box or a valgrind lane never trips it, short
 * enough that a genuine hang is reported rather than waited out. */
#define PUMP_DEADLINE_SECONDS 10.0

/* Fixtures whose byte lengths are the point.  Spelled as escapes rather
 * than as literal characters so the file's own encoding cannot change what
 * is being measured. */
#define E_ACUTE "\xc3\xa9" /* U+00E9, 2 bytes, 1 UTF-16 unit */
#define EURO "\xe2\x82\xac" /* U+20AC, 3 bytes, 1 UTF-16 unit */
#define CLEF "\xf0\x9d\x84\x9e" /* U+1D11E, 4 bytes, 2 UTF-16 units */

static char script_path[PATH_MAX];
/* Short of PATH_MAX by the longest name any case joins onto it, so that
 * every snprintf() below is provably not a truncation and the compiler can
 * see it. */
static char workdir[PATH_MAX - 64];
static char record_path[PATH_MAX];
static char file_path[PATH_MAX];

static double monotonic_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ============================== positions ============================== */

/* Pure ASCII is the one case where every space agrees, which is exactly why
 * it proves nothing on its own -- it is here as the control. */
static void test_position_ascii_is_the_byte_offset(void)
{
	static const char row[] = "int main(void)";
	size_t len = sizeof(row) - 1;

	CHECK(lsp_pos_utf16_from_byte(row, len, 0) == 0);
	CHECK(lsp_pos_utf16_from_byte(row, len, 4) == 4);
	CHECK(lsp_pos_byte_from_utf16(row, len, 4) == 4);
	CHECK(lsp_pos_encode(LSP_POSITION_UTF8, row, len, 9) == 9);
	CHECK(lsp_pos_decode(LSP_POSITION_UTF8, row, len, 9) == 9);
	CHECK(lsp_pos_encode(LSP_POSITION_UTF16, row, len, 9) == 9);
}

/* Two-, three- and four-byte glyphs on one row.  The four-byte one is the
 * case a naive "count characters" conversion gets wrong: it is one
 * character and TWO UTF-16 code units, and a server told otherwise resolves
 * every position after it one unit early. */
static void test_position_counts_utf16_code_units(void)
{
	static const char row[] = "a" E_ACUTE "b" EURO "c" CLEF "d";
	size_t len = sizeof(row) - 1;

	CHECK(len == 1 + 2 + 1 + 3 + 1 + 4 + 1);
	CHECK(lsp_pos_utf16_from_byte(row, len, 1) == 1); /* after "a" */
	CHECK(lsp_pos_utf16_from_byte(row, len, 3) == 2); /* after e-acute */
	CHECK(lsp_pos_utf16_from_byte(row, len, 4) == 3); /* after "b" */
	CHECK(lsp_pos_utf16_from_byte(row, len, 7) == 4); /* after euro */
	CHECK(lsp_pos_utf16_from_byte(row, len, 8) == 5); /* after "c" */
	CHECK(lsp_pos_utf16_from_byte(row, len, 12) == 7); /* after the clef */
	CHECK(lsp_pos_utf16_from_byte(row, len, len) == 8);

	CHECK(lsp_pos_byte_from_utf16(row, len, 0) == 0);
	CHECK(lsp_pos_byte_from_utf16(row, len, 2) == 3);
	CHECK(lsp_pos_byte_from_utf16(row, len, 4) == 7);
	CHECK(lsp_pos_byte_from_utf16(row, len, 5) == 8);
	CHECK(lsp_pos_byte_from_utf16(row, len, 7) == 12);
	CHECK(lsp_pos_byte_from_utf16(row, len, 8) == len);
}

/* Every glyph start round-trips through both conversions.  Interior bytes
 * deliberately do not, and the rule is stated rather than tolerated: the
 * protocol cannot name a position inside a character, so a byte offset
 * that falls in one is rounded up to the glyph's end. */
static void test_position_round_trips_at_glyph_starts(void)
{
	static const char row[] = E_ACUTE "x" CLEF EURO;
	size_t len = sizeof(row) - 1;
	size_t starts[] = { 0, 2, 3, 7, 10 };
	size_t i;

	for (i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
		size_t units = lsp_pos_utf16_from_byte(row, len, starts[i]);

		CHECKF(lsp_pos_byte_from_utf16(row, len, units) == starts[i],
		    "byte %zu -> %zu units", starts[i], units);
	}
	/* One byte into the clef: the whole glyph is counted. */
	CHECK(lsp_pos_utf16_from_byte(row, len, 4) == 4);
	CHECK(lsp_pos_utf16_from_byte(row, len, 3) == 2);
	/* Half a surrogate pair asks for a position that does not exist;
	 * the answer is the end of the pair, never the middle of a glyph. */
	CHECK(lsp_pos_byte_from_utf16(row, len, 3) == 7);
}

/* Out of range clamps to the end of the line, in both directions and both
 * encodings.  The protocol requires it of a server receiving one, and kg
 * has the same reason: a stale position from a slow answer must land
 * somewhere in the buffer rather than past it. */
static void test_position_clamps_out_of_range(void)
{
	static const char row[] = "ab" EURO;
	size_t len = sizeof(row) - 1;

	CHECK(lsp_pos_utf16_from_byte(row, len, 999) == 3);
	CHECK(lsp_pos_byte_from_utf16(row, len, 999) == len);
	CHECK(lsp_pos_encode(LSP_POSITION_UTF8, row, len, 999) == len);
	CHECK(lsp_pos_decode(LSP_POSITION_UTF8, row, len, 999) == len);
	CHECK(lsp_pos_encode(LSP_POSITION_UTF16, row, len, 999) == 3);
	CHECK(lsp_pos_decode(LSP_POSITION_UTF16, row, len, 999) == len);
	/* An empty line has exactly one position. */
	CHECK(lsp_pos_utf16_from_byte("", 0, 0) == 0);
	CHECK(lsp_pos_utf16_from_byte("", 0, 5) == 0);
	CHECK(lsp_pos_byte_from_utf16("", 0, 5) == 0);
}

/* kg never refuses to show a file, so it must never refuse to say where a
 * position in one is.  A byte that starts no well-formed sequence counts as
 * one unit, which is what a display substituting one replacement character
 * per bad byte does -- and agreeing with the display is what keeps the
 * position pointing where the user is looking. */
static void test_position_counts_malformed_bytes_singly(void)
{
	/* A lone continuation byte, an overlong lead, a truncated
	 * three-byte sequence, and a lead byte that encodes nothing. */
	static const char row[] = "\x80\xc0"
				  "a\xe2\x82\xf8";
	size_t len = sizeof(row) - 1;

	CHECK(len == 6);
	CHECK(lsp_pos_utf16_from_byte(row, len, len) == 6);
	CHECK(lsp_pos_byte_from_utf16(row, len, 6) == len);
	/* A truncated sequence at the end of the line is bytes, not a
	 * glyph: three of them, counted one each. */
	CHECK(lsp_pos_utf16_from_byte(row, len, 3) == 3);
	CHECK(lsp_pos_utf16_from_byte(row, len, 5) == 5);
}

/* ================================ URIs ================================= */

static void check_round_trip(const char *path, const char *want_uri)
{
	char uri[PATH_MAX];
	char back[PATH_MAX];

	CHECKF(lsp_uri_from_path(path, uri, sizeof(uri)), "%s", path);
	CHECKF(strcmp(uri, want_uri) == 0, "%s: got '%s', want '%s'", path, uri,
	    want_uri);
	CHECKF(lsp_uri_to_path(uri, back, sizeof(back)), "%s", uri);
	CHECKF(strcmp(back, path) == 0, "%s: got '%s'", uri, back);
}

static void test_uri_round_trips_ordinary_paths(void)
{
	check_round_trip("/home/u/a b.c", "file:///home/u/a%20b.c");
	check_round_trip("/tmp/x.c", "file:///tmp/x.c");
	check_round_trip("/a-b_c.d~e/f", "file:///a-b_c.d~e/f");
	/* Everything outside the unreserved set is encoded, deliberately
	 * including bytes a laxer encoder would leave alone. */
	check_round_trip("/p/#q?r&s=t", "file:///p/%23q%3Fr%26s%3Dt");
	check_round_trip("/p/a+b,c;d", "file:///p/a%2Bb%2Cc%3Bd");
}

/* A path is bytes, and a UTF-8 name is percent-encoded byte for byte --
 * which is what makes the round trip exact for a name that is not UTF-8
 * either. */
static void test_uri_encodes_non_ascii_byte_for_byte(void)
{
	check_round_trip("/tmp/caf" E_ACUTE ".py", "file:///tmp/caf%C3%A9.py");
	check_round_trip("/tmp/" CLEF, "file:///tmp/%F0%9D%84%9E");
	check_round_trip("/tmp/\xff\xfe", "file:///tmp/%FF%FE");
}

/* A relative path has no URI: a server resolving one against a directory kg
 * never named would answer about a different file. */
static void test_uri_refuses_a_relative_path(void)
{
	char uri[PATH_MAX];

	CHECK(!lsp_uri_from_path("src/main.c", uri, sizeof(uri)));
	CHECK(!lsp_uri_from_path("", uri, sizeof(uri)));
	CHECK(!lsp_uri_from_path(NULL, uri, sizeof(uri)));
	/* A buffer too small is a refusal, not a truncated URI. */
	CHECK(!lsp_uri_from_path("/some/long/path", uri, 12));
}

static void check_rejected(const char *uri)
{
	char out[PATH_MAX];

	out[0] = 'x';
	CHECKF(!lsp_uri_to_path(uri, out, sizeof(out)), "accepted '%s'", uri);
}

/* Everything kg must refuse, and the reason is the same for all of them:
 * the caller of this function opens a file with what comes back. */
static void test_uri_refuses_anything_but_a_local_file(void)
{
	check_rejected("http://example.com/x.c");
	check_rejected("jar:file:///x.c!/y.c");
	check_rejected("/x.c"); /* no scheme at all */
	check_rejected("file:/x.c"); /* no authority component */
	check_rejected("file://otherhost/x.c");
	check_rejected("file://example.com/x.c");
	check_rejected("file://localhostx/x.c");
	check_rejected("file://localhost"); /* an authority and no path */
	check_rejected(NULL);
}

/* Malformed escapes are refused rather than passed through: a `%` left
 * literal is a path that names something else, and `%00` is the classic way
 * a check on a whole name ends up applied to a prefix of it. */
static void test_uri_refuses_malformed_escapes(void)
{
	check_rejected("file:///tmp/%zz");
	check_rejected("file:///tmp/%2");
	check_rejected("file:///tmp/%");
	check_rejected("file:///tmp/a%00b");
	check_rejected("file:///tmp/%GG");
}

/* The two spellings a real server uses, and the case-insensitivity RFC 3986
 * requires of the scheme and the host. */
static void test_uri_accepts_localhost_and_any_case(void)
{
	char out[PATH_MAX];

	CHECK(lsp_uri_to_path("file://localhost/tmp/x.c", out, sizeof(out)));
	CHECK(strcmp(out, "/tmp/x.c") == 0);
	CHECK(lsp_uri_to_path("FILE://LOCALHOST/tmp/x.c", out, sizeof(out)));
	CHECK(strcmp(out, "/tmp/x.c") == 0);
	CHECK(lsp_uri_to_path("file:///tmp/a%20b.c", out, sizeof(out)));
	CHECK(strcmp(out, "/tmp/a b.c") == 0);
	/* Lower-case hex digits decode too, though kg never writes them. */
	CHECK(lsp_uri_to_path("file:///tmp/caf%c3%a9.py", out, sizeof(out)));
	CHECK(strcmp(out, "/tmp/caf" E_ACUTE ".py") == 0);
	/* A result that does not fit is a refusal, not a truncation. */
	CHECK(!lsp_uri_to_path("file:///tmp/x.c", out, 4));
}

/* ========================== the recorded session ======================= */

/* The whole record file, split into lines in place.  Bounded rather than
 * grown: a case that recorded more than this is a case that is not testing
 * what it says it is. */
static char record_text[65536];
static char *record_line[32];
static int record_count;
static struct kg_json *record_doc;

static void record_reset(void)
{
	kg_json_free(record_doc);
	record_doc = NULL;
	record_count = 0;
	record_text[0] = '\0';
	unlink(record_path);
}

static void record_read(void)
{
	FILE *fp = fopen(record_path, "rb");
	size_t got = 0;
	char *p;

	record_count = 0;
	if (fp) {
		got = fread(record_text, 1, sizeof(record_text) - 1, fp);
		fclose(fp);
	}
	record_text[got] = '\0';
	for (p = record_text; *p && record_count < 32;) {
		char *nl = strchr(p, '\n');

		if (!nl) {
			break; /* a line still being written; ignore it */
		}
		*nl = '\0';
		record_line[record_count++] = p;
		p = nl + 1;
	}
}

/* A string node's bytes, or "" for a member the server did not send: every
 * assertion below is a strcmp, and an absent member should fail it rather
 * than crash it. */
static const char *json_text(const struct kg_json_value *v)
{
	const char *s = kg_json_str(v, NULL);

	return s ? s : "";
}

/* The `params` of the i-th recorded notification, with its method through
 * `method`.  One parsed document at a time: the previous is released here,
 * so a case reads its records in order and nothing outlives the assertion
 * it was read for. */
static const struct kg_json_value *record_params(int i, const char **method)
{
	const struct kg_json_value *root;

	kg_json_free(record_doc);
	record_doc = NULL;
	if (i < 0 || i >= record_count) {
		*method = "";
		return NULL;
	}
	record_doc
	    = kg_json_parse(record_line[i], strlen(record_line[i]), NULL);
	root = kg_json_root(record_doc);
	*method = json_text(kg_json_get(root, "method"));
	return kg_json_get(root, "params");
}

/* ---------------------------- the fake server ------------------------- */

struct answer {
	int calls;
};

static void on_reply(struct lsp_client *c, const struct kg_json_value *result,
    const struct kg_json_value *error, void *ctx)
{
	(void)c;
	(void)result;
	(void)error;
	((struct answer *)ctx)->calls++;
}

static struct lsp_client *start_server_argv(
    const char *sync, const char *encoding, bool no_open_close)
{
	const char *argv[16];
	struct kg_spawn_request req = { .stdin_fd = -1 };
	int n = 0;

	record_reset();
	argv[n++] = "python3";
	argv[n++] = script_path;
	argv[n++] = "--mode";
	argv[n++] = "protocol";
	argv[n++] = "--sync";
	argv[n++] = sync;
	argv[n++] = "--position-encoding";
	argv[n++] = encoding;
	argv[n++] = "--record";
	argv[n++] = record_path;
	if (no_open_close) {
		argv[n++] = "--no-open-close";
	}
	argv[n] = NULL;
	req.argv = argv;
	return lsp_client_start(&req, workdir);
}

/* The ordinary server: it wants its documents opened and closed. */
static struct lsp_client *start_server(const char *sync, const char *encoding)
{
	return start_server_argv(sync, encoding, false);
}

static bool pump_until_ready(struct lsp_client *c)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 }; /* 1 ms */

	while (lsp_client_state(c) == LSP_CLIENT_INITIALIZING
	    && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
	return lsp_client_state(c) == LSP_CLIENT_READY;
}

/* Send one request and pump until it is answered, then read the record
 * file.  The fake handles messages in the order they arrive, so every
 * notification kg sent before this request is on disk by the time its reply
 * comes back -- which is what makes "nothing was sent" an assertion rather
 * than a wait. */
static void barrier(struct lsp_client *c)
{
	double deadline = monotonic_seconds() + PUMP_DEADLINE_SECONDS;
	struct timespec nap = { 0, 1000000 };
	struct answer got = { 0 };

	CHECK(lsp_client_request(c, "kg/echo", NULL, 0, on_reply, &got) > 0);
	while (got.calls == 0 && monotonic_seconds() < deadline) {
		(void)lsp_client_poll(c);
		nanosleep(&nap, NULL);
	}
	CHECK(got.calls == 1);
	record_read();
}

/* ------------------------------ the buffer ---------------------------- */

/* The current buffer, holding `lines`, visiting `name` in the case's
 * temporary directory.  The file need not exist: a URI names a path, and a
 * buffer is allowed to visit one nobody has saved yet. */
static void setup_buffer(const char *name, const char *const *lines)
{
	int i;

	free(bcur()->filename);
	bcur()->filename = NULL;
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
	snprintf(file_path, sizeof(file_path), "%s/%s", workdir, name);
	bcur()->filename = strdup(file_path);
	editor_select_syntax_highlight(bcur(), bcur()->filename);
	for (i = 0; lines && lines[i]; i++) {
		editor_insert_row(bcur(), i, lines[i], strlen(lines[i]));
	}
}

static void teardown_buffer(void)
{
	free(bcur()->filename);
	bcur()->filename = NULL;
	free_all_rows();
	undo_free();
}

/* Replace the flat byte range [begin, end) with `text`, as one edit, which
 * is what bumps the buffer's content generation and so what the next
 * lsp_sync_before_request() notices. */
static void replace_bytes(size_t begin, size_t end, const char *text)
{
	struct kg_edit e
	    = kg_edit_internal(bcur(), begin, end, text, strlen(text));

	CHECKF(kg_buffer_replace(&e, NULL) == 1, "[%zu,%zu) := '%s'", begin,
	    end, text);
}

/* ------------------------------ assertions ---------------------------- */

static long long json_at(
    const struct kg_json_value *v, const char *a, const char *b, const char *c)
{
	const struct kg_json_value *n = kg_json_get(v, a);

	n = b ? kg_json_get(n, b) : n;
	n = c ? kg_json_get(n, c) : n;
	return kg_json_int(n, -1);
}

/* The one contentChanges element of a recorded didChange. */
static const struct kg_json_value *only_change(
    const struct kg_json_value *params)
{
	const struct kg_json_value *changes
	    = kg_json_get(params, "contentChanges");

	CHECK(kg_json_len(changes) == 1);
	return kg_json_at(changes, 0);
}

static void check_ranged_change(int index, long long version, long long sl,
    long long sc, long long el, long long ec, const char *text)
{
	const struct kg_json_value *params;
	const struct kg_json_value *change;
	const struct kg_json_value *range;
	const char *method = "";

	params = record_params(index, &method);
	CHECKF(
	    strcmp(method, "textDocument/didChange") == 0, "got '%s'", method);
	CHECK(json_at(params, "textDocument", "version", NULL) == version);
	change = only_change(params);
	range = kg_json_get(change, "range");
	CHECKF(json_at(range, "start", "line", NULL) == sl, "start.line");
	CHECKF(json_at(range, "start", "character", NULL) == sc,
	    "start.character (got %lld, want %lld)",
	    json_at(range, "start", "character", NULL), sc);
	CHECKF(json_at(range, "end", "line", NULL) == el, "end.line");
	CHECKF(json_at(range, "end", "character", NULL) == ec,
	    "end.character (got %lld, want %lld)",
	    json_at(range, "end", "character", NULL), ec);
	CHECKF(strcmp(json_text(kg_json_get(change, "text")), text) == 0,
	    "text (got '%s', want '%s')",
	    json_text(kg_json_get(change, "text")), text);
}

/* =========================== synchronisation =========================== */

static const char *const c_source[] = { "int main(void)", "{", "}", NULL };

/* The first request opens the document: whole text, version 1, and the
 * languageId the server needs to parse it at all. */
static void test_didopen_carries_the_whole_text(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	const struct kg_json_value *params;
	const struct kg_json_value *doc;
	const char *method = "";
	struct kg_buffer_handle buf;
	char want_uri[PATH_MAX];

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 1);
	params = record_params(0, &method);
	CHECK(strcmp(method, "textDocument/didOpen") == 0);
	doc = kg_json_get(params, "textDocument");
	CHECK(lsp_uri_from_path(file_path, want_uri, sizeof(want_uri)));
	CHECK(strcmp(json_text(kg_json_get(doc, "uri")), want_uri) == 0);
	CHECK(strcmp(json_text(kg_json_get(doc, "languageId")), "c") == 0);
	CHECK(kg_json_int(kg_json_get(doc, "version"), -1) == 1);
	CHECK(
	    strcmp(json_text(kg_json_get(doc, "text")), "int main(void)\n{\n}")
	    == 0);
	/* And the module answers the two questions a request builder asks. */
	CHECK(strcmp(lsp_sync_uri(c, buf), want_uri) == 0);
	CHECK(lsp_sync_version(c, buf) == 1);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* A second request against an unedited buffer sends nothing at all.  This
 * is the common case -- most commands do not follow an edit -- and it is
 * the one the generation comparison exists for. */
static void test_unchanged_buffer_sends_nothing(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 1);
	CHECK(lsp_sync_version(c, buf) == 1);

	/* An edit that replaces bytes with the same bytes changes nothing,
	 * so the generation does not move and neither does the server. */
	replace_bytes(0, 3, "int");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);
	CHECK(record_count == 1);
	CHECK(lsp_sync_version(c, buf) == 1);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* One edit shape per case would be six servers; the shapes are what matter,
 * so they share a session and each one's range is asserted against the
 * document as the previous one left it. */
static void test_incremental_change_is_one_range(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	static const char *const lines[] = { "alpha", "beta", "gamma", NULL };
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);

	/* Append at the very end: an empty range at the end of the text. */
	replace_bytes(16, 16, "!");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	/* Insert at the very start: an empty range at the origin. */
	replace_bytes(0, 0, "// ");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	/* Replace inside the middle line. */
	replace_bytes(9, 13, "BETA");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 4);
	check_ranged_change(1, 2, 2, 5, 2, 5, "!");
	check_ranged_change(2, 3, 0, 0, 0, 0, "// ");
	check_ranged_change(3, 4, 1, 0, 1, 4, "BETA");
	CHECK(lsp_sync_version(c, buf) == 4);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* An edit spanning several lines, and one that empties the buffer.  The
 * second is the range that has to reach the end of the last line rather
 * than the start of a line that does not exist. */
static void test_multi_line_and_whole_document_edits(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	static const char *const lines[] = { "one", "two", "three", NULL };
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);

	/* "one\ntwo\nthree" -> "one\nX\nthree": across two line ends. */
	replace_bytes(4, 7, "X");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	/* And now everything, which leaves a buffer with no rows at all. */
	replace_bytes(0, buffer_byte_length(bcur()), "");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 3);
	check_ranged_change(1, 2, 1, 0, 1, 3, "X");
	check_ranged_change(2, 3, 0, 0, 2, 5, "");

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* "aaaa" -> "aaa".  The common prefix is three bytes and the common suffix
 * would also be three, and they describe the same bytes: a diff that let
 * them overlap would send a range ending before it starts.  The clamp is
 * what makes this one deletion of the last byte. */
static void test_overlapping_prefix_and_suffix_are_clamped(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	static const char *const lines[] = { "aaaa", NULL };
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);

	replace_bytes(0, 4, "aaa");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	/* And back the other way: "aaa" -> "aaaaa" is an insertion, not a
	 * replacement of everything. */
	replace_bytes(0, 3, "aaaaa");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 3);
	check_ranged_change(1, 2, 0, 3, 0, 4, "");
	check_ranged_change(2, 3, 0, 3, 0, 3, "aa");

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* Astral characters before the edit point, with a server that counts in
 * UTF-16.  The clef is one character and two code units, so a range
 * measured in characters -- or in bytes -- names the wrong place, and this
 * is the case that says which. */
static void test_utf16_ranges_count_code_units(void)
{
	struct lsp_client *c = start_server("incremental", "utf-16");
	static const char *const lines[]
	    = { CLEF EURO "ab", "x" E_ACUTE "y", NULL };
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_client_caps(c)->position_encoding == LSP_POSITION_UTF16);
	CHECK(lsp_sync_before_request(c, buf) == 0);

	/* Row 0 is 4 + 3 + 1 + 1 bytes; replace the "b" at byte 8, which is
	 * UTF-16 unit 4 (clef 2, euro 1, "a" 1). */
	replace_bytes(8, 9, "B");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	/* Row 1 starts at byte 10; insert after the e-acute, byte 13, which
	 * is unit 2 of that line. */
	replace_bytes(13, 13, "Z");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 3);
	check_ranged_change(1, 2, 0, 4, 0, 5, "B");
	check_ranged_change(2, 3, 1, 2, 1, 2, "Z");

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* An edit inside a glyph is not a thing the protocol can name, so a diff
 * whose common prefix stops between two bytes of one has to walk back to
 * the glyph's start.  "é" -> "è" differs only in the second byte. */
static void test_diff_ends_land_on_glyph_boundaries(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	static const char *const lines[] = { "x" E_ACUTE "y", NULL };
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);

	/* U+00E9 -> U+00E8: c3 a9 -> c3 a8, a one-byte difference in the
	 * middle of a two-byte glyph.  The range must be the whole glyph. */
	replace_bytes(1, 3, "\xc3\xa8");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 2);
	check_ranged_change(1, 2, 0, 1, 0, 3, "\xc3\xa8");

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* The other end of the same rule, and it needs its own case: the one above
 * is satisfied by the PREFIX walk-back alone, because the glyph it edits
 * has an "y" after it that both texts share, so the common suffix stops on
 * a boundary by luck of the fixture.  Take that "y" away and the suffix
 * scan is what runs into the middle of a glyph.
 *
 * "xé" -> "xĩ" is c3 a9 -> c4 a9 at the end of the line: the last byte is
 * common, so a suffix that is not walked back claims it, and what kg sends
 * is a range ending mid-glyph with the text "\xc4" -- one lead byte, not a
 * character, which is not UTF-8 and so not a JSON string a server can
 * decode.  The assertion is the whole glyph, from byte 1 to the end. */
static void test_diff_suffix_walks_back_to_a_glyph_boundary(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	static const char *const lines[] = { "x" E_ACUTE, NULL };
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);

	/* U+00E9 -> U+0129: c3 a9 -> c4 a9, differing in the FIRST byte. */
	replace_bytes(1, 3, "\xc4\xa9");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 2);
	check_ranged_change(1, 2, 0, 1, 0, 3, "\xc4\xa9");

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* The same edit against a full-sync server: no range at all, and the whole
 * document every time.  Which of the two is sent is the server's choice,
 * captured at the handshake, and this is where kg proves it read it. */
static void test_full_sync_sends_the_whole_document(void)
{
	struct lsp_client *c = start_server("full", "utf-8");
	static const char *const lines[] = { "alpha", "beta", NULL };
	const struct kg_json_value *params;
	const struct kg_json_value *change;
	const char *method = "";
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", lines);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_client_caps(c)->sync == LSP_SYNC_FULL);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	replace_bytes(0, 5, "ALPHA");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 2);
	params = record_params(1, &method);
	CHECK(strcmp(method, "textDocument/didChange") == 0);
	CHECK(json_at(params, "textDocument", "version", NULL) == 2);
	change = only_change(params);
	CHECK(kg_json_get(change, "range") == NULL);
	CHECK(
	    strcmp(json_text(kg_json_get(change, "text")), "ALPHA\nbeta") == 0);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* A server wanting no synchronisation at all reads files from disk, and
 * sending it documents it did not ask for is a protocol violation rather
 * than a kindness.  Nothing is sent -- not the didOpen, not the didChange
 * the edit below would otherwise produce, and so not the didClose either.
 *
 * The document is still TRACKED, and that is not a contradiction: the URI
 * is what the caller above builds `textDocument.uri` out of
 * (src/lsp_sync.h), and such a server is asked positional questions like
 * any other.  A table that forgot it would refuse every command against
 * exactly the servers this capability describes. */
static void test_a_server_that_wants_nothing_gets_nothing(void)
{
	struct lsp_client *c = start_server_argv("none", "utf-8", true);
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_client_caps(c)->sync == LSP_SYNC_NONE);
	CHECK(!lsp_client_caps(c)->open_close);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	replace_bytes(0, 3, "INT");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 0);
	CHECK(lsp_sync_uri(c, buf) != NULL);
	/* Version 1 and never incremented: nothing was ever sent to number. */
	CHECK(lsp_sync_version(c, buf) == 1);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* The other half of the same capability: a server that wants the document
 * opened and closed but no changes pushed to it.  It gets exactly one
 * didOpen, and the shadow still follows the buffer -- so the day it is
 * closed, and reopened, the text kg sends is the current one. */
static void test_open_close_without_changes(void)
{
	struct lsp_client *c = start_server("none", "utf-8");
	const struct kg_json_value *params;
	const char *method = "";
	struct kg_buffer_handle buf;

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_client_caps(c)->sync == LSP_SYNC_NONE);
	CHECK(lsp_client_caps(c)->open_close);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	replace_bytes(0, 3, "INT");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 1);
	params = record_params(0, &method);
	CHECK(strcmp(method, "textDocument/didOpen") == 0);
	CHECK(strcmp(json_text(kg_json_get(
			 kg_json_get(params, "textDocument"), "text")),
		  "int main(void)\n{\n}")
	    == 0);
	/* The version was not spent on a change nobody was told about. */
	CHECK(lsp_sync_version(c, buf) == 1);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* Closing tells every server holding the document, and forgets it -- so a
 * later request on the same handle opens it again at version 1 rather than
 * carrying on from a version the server no longer has. */
static void test_close_notifies_and_forgets(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	const struct kg_json_value *params;
	const char *method = "";
	struct kg_buffer_handle buf;
	char want_uri[PATH_MAX];

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);
	replace_bytes(0, 3, "INT");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(lsp_sync_version(c, buf) == 2);

	lsp_sync_close_buffer(buf);
	CHECK(lsp_sync_uri(c, buf) == NULL);
	CHECK(lsp_sync_version(c, buf) == -1);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(record_count == 4);
	params = record_params(2, &method);
	CHECK(strcmp(method, "textDocument/didClose") == 0);
	CHECK(lsp_uri_from_path(file_path, want_uri, sizeof(want_uri)));
	CHECK(strcmp(json_text(kg_json_get(
			 kg_json_get(params, "textDocument"), "uri")),
		  want_uri)
	    == 0);
	params = record_params(3, &method);
	CHECK(strcmp(method, "textDocument/didOpen") == 0);
	CHECK(json_at(params, "textDocument", "version", NULL) == 1);
	CHECK(lsp_sync_version(c, buf) == 1);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* The buffer starts visiting a different file.  C-x C-w is the way a user
 * does it -- write this buffer to that name -- and all it changes is the
 * buffer's `filename`, which is what this case does by hand: the handle,
 * the rows and the content generation are all the same afterwards, and a
 * document keyed on the handle alone would notice nothing.
 *
 * What must happen is the pair: didClose under the name the server was
 * given, didOpen under the new one, at version 1 with the current text.
 * Everything after it -- every didChange, and every request built from
 * lsp_sync_uri() -- then names the file the user is actually editing.  The
 * failure this replaces was silent and destructive: the server went on
 * being told about a file nobody had touched, and a rename answered
 * against it named ranges in that file, which kg would have opened and
 * edited. */
static void test_writing_the_buffer_elsewhere_reopens_the_document(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	const struct kg_json_value *params;
	const char *method = "";
	struct kg_buffer_handle buf;
	char first_uri[PATH_MAX];
	char second_uri[PATH_MAX];
	char second_path[PATH_MAX];

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_uri_from_path(file_path, first_uri, sizeof(first_uri)));
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(strcmp(lsp_sync_uri(c, buf), first_uri) == 0);

	snprintf(second_path, sizeof(second_path), "%s/b.c", workdir);
	CHECK(lsp_uri_from_path(second_path, second_uri, sizeof(second_uri)));
	free(bcur()->filename);
	bcur()->filename = strdup(second_path);
	replace_bytes(0, 3, "INT");
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(strcmp(lsp_sync_uri(c, buf), second_uri) == 0);
	barrier(c);

	CHECK(record_count == 3);
	params = record_params(1, &method);
	CHECK(strcmp(method, "textDocument/didClose") == 0);
	CHECK(strcmp(json_text(kg_json_get(
			 kg_json_get(params, "textDocument"), "uri")),
		  first_uri)
	    == 0);
	params = record_params(2, &method);
	CHECK(strcmp(method, "textDocument/didOpen") == 0);
	CHECK(strcmp(json_text(kg_json_get(
			 kg_json_get(params, "textDocument"), "uri")),
		  second_uri)
	    == 0);
	CHECK(json_at(params, "textDocument", "version", NULL) == 1);
	CHECK(strcmp(json_text(kg_json_get(
			 kg_json_get(params, "textDocument"), "text")),
		  "INT main(void)\n{\n}")
	    == 0);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* The same close, reached the way the editor reaches it: a buffer is killed
 * and the document goes with it, through the event queue rather than
 * through a call anybody wrote at the kill site.
 *
 * The wiring is the whole point of the case.  lsp_sync_install() subscribes
 * to KG_EVENT_BUFFER_KILLED, which is published AFTER the buffer's
 * generation has been bumped (src/bufmgr.c's buf_kill_commit()), so the
 * handle a subscriber receives never resolves -- and must not need to.
 * That is reproduced exactly here: the generation is bumped first, the
 * handle is asserted dead, and only then is the event published and
 * drained.  A subscriber that resolved the handle instead of comparing it
 * would send nothing, and the server would be left holding a document for a
 * buffer that no longer exists and can never be closed, since closing needs
 * the URI only this table still has. */
static void test_a_killed_buffer_closes_its_document(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	const struct kg_json_value *params;
	struct kg_event_reservation res;
	struct kg_buffer_handle buf;
	const char *method = "";
	char want_uri[PATH_MAX];

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("killed.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(lsp_sync_uri(c, buf) != NULL);

	lsp_sync_install();
	bcur()->generation++;
	CHECK(buf_resolve(buf) == NULL);
	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	CHECK(kg_event_publish_lifecycle(&res, kg_event_make_buffer_killed(buf))
	    == KG_EVENT_QUEUED);
	kg_event_drain_safe();

	CHECK(lsp_sync_uri(c, buf) == NULL);
	CHECK(lsp_sync_version(c, buf) == -1);
	barrier(c);

	CHECK(record_count == 2);
	params = record_params(1, &method);
	CHECKF(
	    strcmp(method, "textDocument/didClose") == 0, "got '%s'", method);
	CHECK(lsp_uri_from_path(file_path, want_uri, sizeof(want_uri)));
	CHECK(strcmp(json_text(kg_json_get(
			 kg_json_get(params, "textDocument"), "uri")),
		  want_uri)
	    == 0);

	/* The slot is the fixture's, not a really killed buffer's; put its
	 * generation back so the next case's handle names it again. */
	bcur()->generation--;
	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* A dead buffer is a refusal, not a request sent about a document that no
 * longer exists.  The handle is the only thing that can tell the
 * difference, which is why the entry point takes one. */
static void test_a_dead_handle_is_refused(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	struct kg_buffer_handle stale = { 0, 0, 0 };

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, stale) == -1);
	CHECK(lsp_sync_before_request(NULL, buf_handle(buf_current)) == -1);

	/* A buffer visiting no file has no URI and so no document. */
	free(bcur()->filename);
	bcur()->filename = NULL;
	CHECK(lsp_sync_before_request(c, buf_handle(buf_current)) == -1);
	barrier(c);
	CHECK(record_count == 0);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* A buffer visiting a relative name still gets an absolute URI: the working
 * directory is kg's, and a server told a relative path would resolve it
 * against its own. */
static void test_a_relative_file_name_is_absolutised(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	struct kg_buffer_handle buf;
	char cwd[PATH_MAX - 64];
	char want[PATH_MAX];
	char want_uri[PATH_MAX];

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	free(bcur()->filename);
	bcur()->filename = strdup("relative.c");
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(c));
	CHECK(lsp_sync_before_request(c, buf) == 0);
	barrier(c);

	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	snprintf(want, sizeof(want), "%s/relative.c", cwd);
	CHECK(lsp_uri_from_path(want, want_uri, sizeof(want_uri)));
	CHECK(record_count == 1);
	CHECK(lsp_sync_uri(c, buf) != NULL);
	CHECK(lsp_sync_uri(c, buf)
	    && strcmp(lsp_sync_uri(c, buf), want_uri) == 0);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* The first command after a lazy spawn asks its question before the
 * handshake has settled, which is the ordinary case and not a corner one:
 * src/lsp_server.h starts a server on demand and src/lsp_client.h queues
 * what is sent to it until `initialize` is answered.  Until Stage 5 this
 * module read the not-yet-known capabilities as a refusal -- they have the
 * same shape as one -- and sent nothing, so M-. on a cold server measured a
 * position in a document the server had never been given.
 *
 * Note what is NOT pumped before the request here: the client is still
 * INITIALIZING when lsp_sync_before_request() runs, exactly as it is under
 * M-. .  The document is registered there and the didOpen is held -- what
 * the server wants is not known yet -- and goes out the moment the
 * handshake settles, from the ready hook, ahead of everything the client
 * queued behind it.  The record file's single line and the barrier's own
 * reply together say that it arrived; M-.'s own request is a deferred one,
 * whose place in that queue is asserted by test_lsp_client.c's
 * test_a_deferred_request_stays_behind_a_notification(). */
static void test_sync_before_the_handshake_still_opens(void)
{
	struct lsp_client *c = start_server("incremental", "utf-8");
	struct kg_buffer_handle buf;
	const char *method = "";

	CHECK(c != NULL);
	if (!c) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(lsp_client_state(c) == LSP_CLIENT_INITIALIZING);
	CHECK(lsp_sync_before_request(c, buf) == 0);
	CHECK(lsp_sync_uri(c, buf) != NULL);
	barrier(c);

	CHECK(record_count == 1);
	(void)record_params(0, &method);
	CHECK(strcmp(method, "textDocument/didOpen") == 0);

	lsp_sync_drop_client(c);
	lsp_client_dispose(c, 200);
	teardown_buffer();
}

/* A client that is gone takes its documents with it, silently: a didClose
 * queued for a dead server is a message nobody reads, and a shadow left
 * behind is a pointer into freed memory the next lookup would compare
 * against. */
static void test_dropping_a_client_forgets_its_documents(void)
{
	struct lsp_client *first = start_server("incremental", "utf-8");
	struct lsp_client *second;
	struct kg_buffer_handle buf;

	CHECK(first != NULL);
	if (!first) {
		return;
	}
	setup_buffer("a.c", c_source);
	buf = buf_handle(buf_current);
	CHECK(pump_until_ready(first));
	CHECK(lsp_sync_before_request(first, buf) == 0);
	replace_bytes(0, 3, "INT");
	CHECK(lsp_sync_before_request(first, buf) == 0);
	CHECK(lsp_sync_version(first, buf) == 2);

	lsp_sync_drop_client(first);
	CHECK(lsp_sync_uri(first, buf) == NULL);
	lsp_client_dispose(first, 200);

	/* A second server for the same buffer opens it from scratch: the
	 * table is keyed by (client, buffer), not by buffer. */
	second = start_server("incremental", "utf-8");
	CHECK(second != NULL);
	if (second) {
		CHECK(pump_until_ready(second));
		CHECK(lsp_sync_before_request(second, buf) == 0);
		barrier(second);
		CHECK(record_count == 1);
		CHECK(lsp_sync_version(second, buf) == 1);
		lsp_sync_drop_client(second);
		lsp_client_dispose(second, 200);
	}
	teardown_buffer();
}

/* ------------------------------- harness ------------------------------ */

/* Where test/fake_lsp_server.py is, given how this binary was invoked:
 * beside it in test/ for `make check` and for a hand-run valgrind, with the
 * repo-relative path as the fallback for a runner that renames the
 * binary. */
static void resolve_script_path(const char *argv0)
{
	const char *slash = strrchr(argv0, '/');
	int dir_len = slash ? (int)(slash - argv0) + 1 : 0;
	char resolved[PATH_MAX];

	snprintf(script_path, sizeof(script_path), "%.*sfake_lsp_server.py",
	    dir_len, argv0);
	if (access(script_path, R_OK) != 0) {
		snprintf(script_path, sizeof(script_path),
		    "test/fake_lsp_server.py");
	}
	if (realpath(script_path, resolved)) {
		snprintf(script_path, sizeof(script_path), "%s", resolved);
	}
}

static bool make_workdir(void)
{
	char template[] = "/tmp/kg-lsp-sync-XXXXXX";
	char resolved[PATH_MAX];
	const char *dir;

	if (!mkdtemp(template)) {
		return false;
	}
	/* realpath, because a /tmp that is itself a symlink would make the
	 * URIs these cases expect disagree with the ones kg builds. */
	dir = realpath(template, resolved) ? resolved : template;
	if (strlen(dir) >= sizeof(workdir)) {
		return false;
	}
	memcpy(workdir, dir, strlen(dir) + 1);
	snprintf(record_path, sizeof(record_path), "%s/record.jsonl", workdir);
	return true;
}

static void remove_workdir(void)
{
	char command[PATH_MAX + 32];

	snprintf(command, sizeof(command), "rm -rf '%s'", workdir);
	if (system(command) != 0) {
		fprintf(
		    stderr, "test_lsp_sync: could not remove %s\n", workdir);
	}
}

int main(int argc, char **argv)
{
	(void)argc;
	resolve_script_path(argv[0]);
	if (access(script_path, R_OK) != 0) {
		fprintf(stderr,
		    "test_lsp_sync: cannot find fake_lsp_server.py "
		    "(tried '%s')\n",
		    script_path);
		return 1;
	}
	if (!make_workdir()) {
		fprintf(stderr, "test_lsp_sync: mkdtemp failed\n");
		return 1;
	}

	RUN(test_position_ascii_is_the_byte_offset);
	RUN(test_position_counts_utf16_code_units);
	RUN(test_position_round_trips_at_glyph_starts);
	RUN(test_position_clamps_out_of_range);
	RUN(test_position_counts_malformed_bytes_singly);

	RUN(test_uri_round_trips_ordinary_paths);
	RUN(test_uri_encodes_non_ascii_byte_for_byte);
	RUN(test_uri_refuses_a_relative_path);
	RUN(test_uri_refuses_anything_but_a_local_file);
	RUN(test_uri_refuses_malformed_escapes);
	RUN(test_uri_accepts_localhost_and_any_case);

	RUN(test_didopen_carries_the_whole_text);
	RUN(test_unchanged_buffer_sends_nothing);
	RUN(test_incremental_change_is_one_range);
	RUN(test_multi_line_and_whole_document_edits);
	RUN(test_overlapping_prefix_and_suffix_are_clamped);
	RUN(test_utf16_ranges_count_code_units);
	RUN(test_diff_ends_land_on_glyph_boundaries);
	RUN(test_diff_suffix_walks_back_to_a_glyph_boundary);
	RUN(test_full_sync_sends_the_whole_document);
	RUN(test_a_server_that_wants_nothing_gets_nothing);
	RUN(test_open_close_without_changes);
	RUN(test_close_notifies_and_forgets);
	RUN(test_writing_the_buffer_elsewhere_reopens_the_document);
	RUN(test_a_killed_buffer_closes_its_document);
	RUN(test_a_dead_handle_is_refused);
	RUN(test_a_relative_file_name_is_absolutised);
	RUN(test_sync_before_the_handshake_still_opens);
	RUN(test_dropping_a_client_forgets_its_documents);

	record_reset();
	remove_workdir();
	return test_summary();
}
