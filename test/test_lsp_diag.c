/* test_lsp_diag.c — the diagnostics store, and the hover renderer.
 *
 * Two modules in one binary because they are the two halves of the same
 * slice and their link is identical: both are the command layer, so both
 * need the whole editor behind them (Makefile's EXTRA_lsp_diag).
 *
 * lsp_diag_publish() is the whole of what a `publishDiagnostics`
 * notification does (src/lsp_diag.h), so every store case drives it
 * directly from a parsed document, with no server and no client in the
 * way.  What that leaves for the PTY cases is the listing being displayed
 * and RET landing in it: this binary's window entry points are
 * stubs_win.c's and its editor_goto_line_direct() is stubs_buffer.c's, so
 * point never actually moves here.
 *
 * lsp_hover_render() is pure, and is where a hover client is either right
 * about the protocol's three `contents` shapes or shows a reader a JSON
 * fragment.
 */

#include "../src/decor.h"
#include "../src/def.h"
#include "../src/lsp_client.h"
#include "../src/lsp_diag.h"
#include "../src/lsp_hover.h"
#include "../src/lsp_json.h"
#include "../src/marker.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

/* The file every store case publishes about.  Absolute, so
 * lsp_sync_abs_path() hands it back unchanged and a buffer named this way
 * is the buffer the publish is about (src/lsp_sync.h). */
#define DIAG_PATH "/tmp/kg-test-diag/main.c"
#define DIAG_URI "file:///tmp/kg-test-diag/main.c"

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	memset(&editor, 0, sizeof(editor));
	buf_count = 1;
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
	lsp_diag_test_reset();
}

static void teardown(void)
{
	lsp_diag_test_reset();
	undo_free();
	free_all_rows();
	kg_marker_store_free(bcur());
	kg_decor_store_free(bcur());
	free(bcur()->filename);
	bcur()->filename = NULL;
}

/* Publish one document's worth of JSON.  The document is freed before
 * returning, which is also the assertion that nothing in the store
 * borrows from it: every string a publish keeps is a copy. */
static void publish(const char *json, enum lsp_position_encoding enc)
{
	struct lsp_json *doc = lsp_json_parse(json, strlen(json), NULL);

	CHECK(doc != NULL);
	if (!doc) {
		return;
	}
	lsp_diag_publish(lsp_json_root(doc), enc);
	lsp_json_free(doc);
}

/* Give the current buffer a file name and some text. */
static void visit(const char *path, const char *text)
{
	free(bcur()->filename);
	bcur()->filename = strdup(path);
	buf_append_special_text(buf_current, text, strlen(text));
}

/* ------------------------------- the store ---------------------------- */

/* What one publish leaves behind: the URI decoded to a path, the range's
 * start, the severity as the protocol numbered it, and the message. */
static void test_a_publish_stores_what_it_carried(void)
{
	const char *path = NULL;
	const char *message = NULL;
	long long character = -1;
	int line = -1;
	int severity = -1;

	setup();
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":2,\"character\":6},"
		"\"end\":{\"line\":2,\"character\":9}},"
		"\"severity\":1,\"message\":\"expected ';'\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 1);
	CHECK(lsp_diag_test_row(
	    0, &path, &line, &character, &severity, &message));
	CHECK(path && strcmp(path, DIAG_PATH) == 0);
	CHECK(line == 2 && character == 6);
	CHECK(severity == LSP_DIAG_ERROR);
	CHECK(message && strcmp(message, "expected ';'") == 0);
	teardown();
}

/* A publish replaces what that file had; it never merges.  That is the
 * protocol's own semantics, and getting it wrong is how a fixed problem
 * stays on screen forever. */
static void test_a_publish_replaces_the_previous_one(void)
{
	const char *message = NULL;

	setup();
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"first\"},"
		"{\"range\":{\"start\":{\"line\":1,\"character\":0}},"
		"\"severity\":2,\"message\":\"second\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 2);
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":5,\"character\":0}},"
		"\"severity\":2,\"message\":\"only this one now\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 1);
	CHECK(lsp_diag_test_row(0, NULL, NULL, NULL, NULL, &message));
	CHECK(message && strcmp(message, "only this one now") == 0);
	teardown();
}

/* An empty list is the server taking everything back. */
static void test_an_empty_publish_clears_the_file(void)
{
	setup();
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"gone soon\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 1);
	publish(
	    "{\"uri\":\"" DIAG_URI "\",\"diagnostics\":[]}", LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 0);
	teardown();
}

/* A publish about a version older than the one already stored describes
 * text the server has since been sent a newer copy of, and its positions
 * were measured against that older text. */
static void test_a_stale_version_is_dropped(void)
{
	const char *message = NULL;

	setup();
	publish("{\"uri\":\"" DIAG_URI "\",\"version\":7,\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"about version 7\"}]}",
	    LSP_POSITION_UTF8);
	publish("{\"uri\":\"" DIAG_URI "\",\"version\":6,\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"about version 6\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 1);
	CHECK(lsp_diag_test_row(0, NULL, NULL, NULL, NULL, &message));
	CHECK(message && strcmp(message, "about version 7") == 0);
	/* The same version again is not stale: a server may republish. */
	publish("{\"uri\":\"" DIAG_URI "\",\"version\":7,\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"again\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row(0, NULL, NULL, NULL, NULL, &message));
	CHECK(message && strcmp(message, "again") == 0);
	teardown();
}

/* A publish carrying no version at all is what a server that does not
 * track them sends, and is always taken. */
static void test_a_publish_without_a_version_is_never_stale(void)
{
	const char *message = NULL;

	setup();
	publish("{\"uri\":\"" DIAG_URI "\",\"version\":9,\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"versioned\"}]}",
	    LSP_POSITION_UTF8);
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"unversioned\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row(0, NULL, NULL, NULL, NULL, &message));
	CHECK(message && strcmp(message, "unversioned") == 0);
	teardown();
}

/* A URI naming something kg cannot open is a server to ignore rather than
 * a file to guess at. */
static void test_a_uri_kg_cannot_open_is_ignored(void)
{
	setup();
	publish("{\"uri\":\"http://example.invalid/main.c\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":1,\"message\":\"nowhere\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 0);
	publish("{\"diagnostics\":[]}", LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 0);
	teardown();
}

/* The listing's rows are in file order, then position order within a
 * file, whatever order the servers published in -- which is the order a
 * reader works through them, and the order next-error walks. */
static void test_rows_are_ordered_by_file_then_position(void)
{
	const char *path = NULL;
	long long character = -1;
	int line = -1;
	size_t i;
	static const struct {
		const char *path;
		int line;
		long long character;
	} want[] = {
		{ "/tmp/kg-test-diag/a.c", 1, 0 },
		{ "/tmp/kg-test-diag/a.c", 4, 2 },
		{ "/tmp/kg-test-diag/a.c", 4, 9 },
		{ "/tmp/kg-test-diag/b.c", 0, 0 },
	};

	setup();
	publish("{\"uri\":\"file:///tmp/kg-test-diag/b.c\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
		"\"severity\":2,\"message\":\"b\"}]}",
	    LSP_POSITION_UTF8);
	publish("{\"uri\":\"file:///tmp/kg-test-diag/a.c\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":4,\"character\":9}},"
		"\"severity\":1,\"message\":\"third\"},"
		"{\"range\":{\"start\":{\"line\":1,\"character\":0}},"
		"\"severity\":1,\"message\":\"first\"},"
		"{\"range\":{\"start\":{\"line\":4,\"character\":2}},"
		"\"severity\":1,\"message\":\"second\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(lsp_diag_test_row_count() == 4);
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (!lsp_diag_test_row(
			i, &path, &line, &character, NULL, NULL)) {
			CHECKF(false, "row %zu is missing", i);
			continue;
		}
		CHECKF(path && strcmp(path, want[i].path) == 0
			&& line == want[i].line
			&& character == want[i].character,
		    "row %zu is %s:%d:%lld", i, path ? path : "(none)", line,
		    character);
	}
	teardown();
}

/* ------------------------------- painting ----------------------------- */

/* The decoration spans this file's diagnostics put in the current buffer,
 * in store order.  Returns how many there were. */
static size_t painted_spans(struct kg_decor_query_span *out, size_t max)
{
	struct kg_decor_query q;
	struct kg_decor_query_span span;
	size_t n = 0;

	kg_decor_query_begin(&q, bcur(), 0, buffer_byte_length(bcur()) + 1);
	while (n < max && kg_decor_query_next(&q, &span)) {
		out[n++] = span;
	}
	return n;
}

/* A publish paints the buffer visiting the file, and the range it paints
 * is the protocol's characters decoded against that row's real bytes.
 *
 * The fixture's second line starts with a two-byte "ä", so a UTF-16
 * character offset and a byte offset disagree by one from there on: this
 * is the mirror of the encoding src/xref.c applies on the way out, and a
 * client that skipped it would mark the byte before the identifier. */
static void test_a_publish_paints_the_buffer_visiting_the_file(void)
{
	struct kg_decor_query_span spans[4];
	size_t n;

	setup();
	visit(DIAG_PATH, "int a;\nä = target;\n");
	/* Line 1 is "ä = target;": "target" starts at UTF-16 character 4
	 * and at byte 5, and is six units long either way. */
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":1,\"character\":4},"
		"\"end\":{\"line\":1,\"character\":10}},"
		"\"severity\":1,\"message\":\"undeclared\"}]}",
	    LSP_POSITION_UTF16);
	n = painted_spans(spans, 4);
	CHECK(n == 1);
	if (n == 1) {
		/* Line 0 is "int a;" plus its newline: seven bytes. */
		CHECK(spans[0].start == 7 + 5);
		CHECK(spans[0].end == 7 + 11);
		CHECK(spans[0].face == KG_DECOR_FACE_WARNING);
	}
	/* The same answer counted in UTF-8 is the same span. */
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":1,\"character\":5},"
		"\"end\":{\"line\":1,\"character\":11}},"
		"\"severity\":1,\"message\":\"undeclared\"}]}",
	    LSP_POSITION_UTF8);
	n = painted_spans(spans, 4);
	CHECK(n == 1);
	if (n == 1) {
		CHECK(spans[0].start == 7 + 5 && spans[0].end == 7 + 11);
	}
	/* And an empty publish takes the marks back with the diagnostics. */
	publish(
	    "{\"uri\":\"" DIAG_URI "\",\"diagnostics\":[]}", LSP_POSITION_UTF8);
	CHECK(painted_spans(spans, 4) == 0);
	teardown();
}

/* An empty range is a real answer -- "here, before this character" -- and
 * a zero-width mark is not visible, so it covers the byte it points at. */
static void test_an_empty_range_still_marks_one_byte(void)
{
	struct kg_decor_query_span spans[2];

	setup();
	visit(DIAG_PATH, "int a;\n");
	publish("{\"uri\":\"" DIAG_URI "\",\"diagnostics\":["
		"{\"range\":{\"start\":{\"line\":0,\"character\":4},"
		"\"end\":{\"line\":0,\"character\":4}},"
		"\"severity\":2,\"message\":\"unused\"}]}",
	    LSP_POSITION_UTF8);
	CHECK(painted_spans(spans, 2) == 1);
	CHECK(spans[0].start == 4 && spans[0].end == 5);
	teardown();
}

/* --------------------------- hover rendering -------------------------- */

/* Render `json`'s root as Hover.contents would be, into a caller's
 * buffer. */
static size_t render(const char *json, char *out, size_t out_size)
{
	struct lsp_json *doc = lsp_json_parse(json, strlen(json), NULL);
	size_t len;

	CHECK(doc != NULL);
	if (!doc) {
		out[0] = '\0';
		return 0;
	}
	len = lsp_hover_render(lsp_json_root(doc), out, out_size);
	lsp_json_free(doc);
	return len;
}

/* The current shape: a MarkupContent object, whose text is under
 * `value`. */
static void test_hover_renders_markup_content(void)
{
	char out[256];

	CHECK(render("{\"kind\":\"plaintext\",\"value\":\"int f(int v)\"}", out,
		  sizeof(out))
	    == 12);
	CHECK(strcmp(out, "int f(int v)") == 0);
}

/* The oldest shape: a bare string. */
static void test_hover_renders_a_bare_string(void)
{
	char out[256];

	CHECK(render("\"just text\"", out, sizeof(out)) == 9);
	CHECK(strcmp(out, "just text") == 0);
}

/* The middle shape: an array of MarkedString, each element either a
 * string or a {language, value} object, joined one per line. */
static void test_hover_renders_marked_string_arrays(void)
{
	char out[256];

	CHECK(render("[{\"language\":\"c\",\"value\":\"int f(int v)\"},"
		     "\"Returns v plus one.\"]",
		  out, sizeof(out))
	    > 0);
	CHECK(strcmp(out, "int f(int v)\nReturns v plus one.") == 0);
}

/* A server with nothing to say says null, and that is not an error. */
static void test_hover_renders_nothing_for_null(void)
{
	char out[256];

	CHECK(render("null", out, sizeof(out)) == 0);
	CHECK(out[0] == '\0');
	CHECK(render("{}", out, sizeof(out)) == 0);
}

/* Markdown is neutralised, not rendered: fences, backticks, heading
 * markers and horizontal rules are punctuation a reader of plain text
 * would otherwise read as text.  Runs of blank lines collapse to one and
 * the trailing ones go, so the first line of the answer is the answer. */
static void test_hover_strips_markdown_noise(void)
{
	char out[512];

	CHECK(render("{\"kind\":\"markdown\",\"value\":"
		     "\"### function `f`\\n\\n---\\n\\n"
		     "```c\\nint f(int v)\\n```\\n\\n\\n\"}",
		  out, sizeof(out))
	    > 0);
	CHECK(strcmp(out, "function f\n\nint f(int v)") == 0);
	/* A `#` that is not a heading marker stays where it is. */
	CHECK(render("\"#include <stdio.h>\"", out, sizeof(out)) > 0);
	CHECK(strcmp(out, "#include <stdio.h>") == 0);
}

/* A control byte in a server's prose is one byte of a listing row or of
 * an echo-area line, and stays one. */
static void test_hover_neutralises_control_bytes(void)
{
	char out[64];

	CHECK(render("\"a\\tb\\u0007c\"", out, sizeof(out)) == 5);
	CHECK(strcmp(out, "a b c") == 0);
}

/* A refused buffer is refused, not half-filled. */
static void test_hover_render_takes_no_room(void)
{
	char out[8];

	CHECK(lsp_hover_render(NULL, out, 0) == 0);
	CHECK(
	    render("\"much longer than eight bytes\"", out, sizeof(out)) == 7);
	CHECK(strcmp(out, "much lo") == 0);
}

int main(void)
{
	RUN(test_a_publish_stores_what_it_carried);
	RUN(test_a_publish_replaces_the_previous_one);
	RUN(test_an_empty_publish_clears_the_file);
	RUN(test_a_stale_version_is_dropped);
	RUN(test_a_publish_without_a_version_is_never_stale);
	RUN(test_a_uri_kg_cannot_open_is_ignored);
	RUN(test_rows_are_ordered_by_file_then_position);
	RUN(test_a_publish_paints_the_buffer_visiting_the_file);
	RUN(test_an_empty_range_still_marks_one_byte);

	RUN(test_hover_renders_markup_content);
	RUN(test_hover_renders_a_bare_string);
	RUN(test_hover_renders_marked_string_arrays);
	RUN(test_hover_renders_nothing_for_null);
	RUN(test_hover_strips_markdown_noise);
	RUN(test_hover_neutralises_control_bytes);
	RUN(test_hover_render_takes_no_room);
	return test_summary();
}
