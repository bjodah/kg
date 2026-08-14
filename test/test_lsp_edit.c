/* test_lsp_edit.c — the WorkspaceEdit machinery behind M-x lsp-rename
 * (src/lsp_edit.h), and the string arithmetic behind M-TAB
 * (src/lsp_complete.h).
 *
 * The commands themselves are PTY territory: they need a server, a poll
 * site and a prompt, and a native test that faked all three would be
 * testing the fake (test/pty/lsp-rename-*.yaml,
 * test/pty/lsp-completion-*.yaml).  What is native here is everything
 * between the reply and the buffer -- reading both shapes a WorkspaceEdit
 * comes in, ordering edits so the later ones are applied first, refusing
 * ones that overlap, decoding the protocol's characters into byte
 * offsets, and the single-replacement application that makes a rename one
 * undo record.
 *
 * This links the whole editor (Makefile's EXTRA_lsp_edit) because
 * applying an edit reaches the buffer table, the edit gateway and the
 * undo stack; the fixtures below build a real buffer and read it back.
 */

#include "../src/def.h"
#include "../src/json.h"
#include "../src/lsp_complete.h"
#include "../src/lsp_edit.h"
#include "../src/marker.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

/* The path the fixture buffer visits.  Absolute, because that is what a
 * URI decodes to and what lsp_sync_abs_path() answers for a buffer
 * already holding an absolute name; it need not exist, since a buffer
 * that is open is never stat()ed. */
#define FIXTURE_PATH "/kg-test/lsp-edit.c"
#define FIXTURE_URI "file:///kg-test/lsp-edit.c"

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	undo_free();
	free_all_rows();
	kg_marker_store_free(bcur());
	free(bcur()->filename);
	bcur()->filename = NULL;
}

/* Give the fixture buffer its lines and the name a URI resolves to.
 * buf_append_special_text() splits on newlines itself, which is what
 * makes this one call per fixture. */
static void fixture(const char *text)
{
	setup();
	buf_append_special_text(buf_current, text, strlen(text));
	free(bcur()->filename);
	bcur()->filename = strdup(FIXTURE_PATH);
	bcur()->dirty = 0;
}

/* The whole buffer as one string, rows joined by newlines.  Static
 * storage: every fixture here is a few dozen bytes. */
static const char *buffer_text(void)
{
	static char out[512];
	size_t n = 0;
	int i;

	for (i = 0; i < bcur()->numrows && n < sizeof(out) - 2; i++) {
		size_t len = (size_t)bcur()->row[i].size;

		if (len > sizeof(out) - 2 - n) {
			len = sizeof(out) - 2 - n;
		}
		memcpy(out + n, bcur()->row[i].chars, len);
		n += len;
		if (i + 1 < bcur()->numrows) {
			out[n++] = '\n';
		}
	}
	out[n] = '\0';
	return out;
}

/* Parse a literal and hand the reader its root, so a case reads as the
 * JSON a server would send.  The document is freed here: everything the
 * reader keeps is its own copy, which is the contract the reply callback
 * depends on. */
static struct lsp_workspace_edit *read_edit(const char *json)
{
	struct kg_json *doc = kg_json_parse(json, strlen(json), NULL);
	struct lsp_workspace_edit *edit;

	if (!doc) {
		return NULL;
	}
	edit = lsp_workspace_edit_read(kg_json_root(doc));
	kg_json_free(doc);
	return edit;
}

/* ------------------------------- reading ------------------------------ */

static void test_reads_the_changes_shape(void)
{
	struct lsp_workspace_edit *edit = read_edit(
	    "{\"changes\":{\"" FIXTURE_URI "\":[{\"range\":{\"start\":{"
	    "\"line\":1,\"character\":"
	    "2},\"end\":{\"line\":1,"
	    "\"character\":5}},"
	    "\"newText\":\"xy\"}]}}");

	CHECK(edit != NULL);
	if (!edit) {
		return;
	}
	CHECK(edit->file_count == 1);
	CHECK(edit->item_count == 1);
	CHECK(edit->dropped == 0);
	CHECK(strcmp(edit->files[0].path, FIXTURE_PATH) == 0);
	CHECK(edit->items[0].range.start_line == 1);
	CHECK(edit->items[0].range.start_char == 2);
	CHECK(edit->items[0].range.end_char == 5);
	CHECK(edit->items[0].len == 2);
	CHECK(edit->items[0].text && strcmp(edit->items[0].text, "xy") == 0);
	lsp_workspace_edit_free(edit);
}

/* The other shape, and the one a modern server sends: an array of
 * TextDocumentEdit objects whose identifiers carry a version, which is
 * kept and checked against the document the server was told about. */
static void test_reads_the_document_changes_shape(void)
{
	struct lsp_workspace_edit *edit = read_edit(
	    "{\"documentChanges\":[{\"textDocument\":{\"uri\":\"" FIXTURE_URI
	    "\",\"version\":7},\"edits\":[{\"range\":{\"start\":{"
	    "\"line\":0,\"character\":0},\"end\":{\"line\":0,"
	    "\"character\":1}},\"newText\":\"A\"},{\"range\":{"
	    "\"start\":{\"line\":2,\"character\":0},\"end\":{"
	    "\"line\":2,\"character\":1}},\"newText\":\"B\"}]}]}");

	CHECK(edit != NULL);
	if (!edit) {
		return;
	}
	CHECK(edit->file_count == 1);
	CHECK(edit->item_count == 2);
	CHECK(edit->dropped == 0);
	CHECK(edit->items[1].range.start_line == 2);
	CHECK(edit->files[0].version == 7);
	lsp_workspace_edit_free(edit);
}

/* An answer that names no version, in either shape, is one nothing can be
 * compared against: -1, which is not a version any document is at. */
static void test_an_absent_version_is_not_a_version(void)
{
	struct lsp_workspace_edit *changes = read_edit(
	    "{\"changes\":{\"" FIXTURE_URI "\":[{\"range\":{\"start\":{"
	    "\"line\":0,\"character\":0},\"end\":{\"line\":0,"
	    "\"character\":1}},\"newText\":\"A\"}]}}");
	struct lsp_workspace_edit *null_version = read_edit(
	    "{\"documentChanges\":[{\"textDocument\":{\"uri\":\"" FIXTURE_URI
	    "\",\"version\":null},\"edits\":[{\"range\":{\"start\":{"
	    "\"line\":0,\"character\":0},\"end\":{\"line\":0,"
	    "\"character\":1}},\"newText\":\"A\"}]}]}");

	CHECK(changes != NULL);
	CHECK(null_version != NULL);
	if (changes) {
		CHECK(changes->files[0].version == -1);
		lsp_workspace_edit_free(changes);
	}
	if (null_version) {
		CHECK(null_version->files[0].version == -1);
		lsp_workspace_edit_free(null_version);
	}
}

/* A document with an empty edit array is a document with nothing to do.
 * It is not registered as a file at all -- which is what stops the
 * application opening a buffer onto it, and stops it being counted as an
 * edit that could not be made. */
static void test_an_empty_edit_array_is_not_a_file(void)
{
	struct lsp_workspace_edit *edit = read_edit(
	    "{\"changes\":{\"" FIXTURE_URI "\":[{\"range\":{\"start\":{"
	    "\"line\":0,\"character\":0},\"end\":{\"line\":0,"
	    "\"character\":1}},\"newText\":\"A\"}],"
	    "\"file:///kg-test/untouched.c\":[]}}");

	CHECK(edit != NULL);
	if (!edit) {
		return;
	}
	CHECK(edit->file_count == 1);
	CHECK(edit->item_count == 1);
	CHECK(edit->dropped == 0);
	CHECK(strcmp(edit->files[0].path, FIXTURE_PATH) == 0);
	lsp_workspace_edit_free(edit);
}

/* documentChanges wins when a server sends both, which is what the
 * specification says a client must do. */
static void test_document_changes_win_over_changes(void)
{
	struct lsp_workspace_edit *edit = read_edit(
	    "{\"changes\":{\"" FIXTURE_URI
	    "\":[{\"range\":{\"start\":{\"line\":9,\"character\":0},"
	    "\"end\":{\"line\":9,\"character\":1}},\"newText\":"
	    "\"old\"}]},\"documentChanges\":[{\"textDocument\":{"
	    "\"uri\":\"" FIXTURE_URI
	    "\"},\"edits\":[{\"range\":{\"start\":{\"line\":1,"
	    "\"character\":0},\"end\":{\"line\":1,\"character\":1}},"
	    "\"newText\":\"new\"}]}]}");

	CHECK(edit != NULL);
	if (!edit) {
		return;
	}
	CHECK(edit->item_count == 1);
	CHECK(edit->items[0].range.start_line == 1);
	CHECK(edit->items[0].text && strcmp(edit->items[0].text, "new") == 0);
	lsp_workspace_edit_free(edit);
}

/* A create/rename/delete of a file is counted and named, never
 * performed.  The text edits beside it are still read. */
static void test_resource_operations_are_counted_not_applied(void)
{
	struct lsp_workspace_edit *edit = read_edit(
	    "{\"documentChanges\":[{\"kind\":\"create\",\"uri\":"
	    "\"file:///kg-test/"
	    "new.c\"},{\"textDocument\":{\"uri\":\"" FIXTURE_URI
	    "\"},\"edits\":[{\"range\":{\"start\":{\"line\":0,"
	    "\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
	    "\"newText\":\"A\"}]},{\"kind\":\"delete\",\"uri\":"
	    "\"file:///kg-test/old.c\"}]}");

	CHECK(edit != NULL);
	if (!edit) {
		return;
	}
	CHECK(edit->resource_ops == 2);
	CHECK(strcmp(edit->resource_kind, "create") == 0);
	CHECK(edit->item_count == 1);
	CHECK(edit->file_count == 1);
	lsp_workspace_edit_free(edit);
}

/* A URI that is not a local file is an edit kg cannot make, and the count
 * of those is what stops the whole answer being applied. */
static void test_unreadable_uris_are_dropped(void)
{
	struct lsp_workspace_edit *edit
	    = read_edit("{\"changes\":{\"https://example.invalid/a.c\":[{"
			"\"range\":{\"start\":{\"line\":0,\"character\":0},"
			"\"end\":{\"line\":0,\"character\":1}},\"newText\":"
			"\"x\"}]}}");

	CHECK(edit != NULL);
	if (!edit) {
		return;
	}
	CHECK(edit->item_count == 0);
	CHECK(edit->dropped == 1);
	lsp_workspace_edit_free(edit);
}

static void test_reading_refusals(void)
{
	struct lsp_workspace_edit *edit;
	struct lsp_edit_range range;
	struct kg_json *doc;

	CHECK(read_edit("null") == NULL);
	CHECK(read_edit("[]") == NULL);
	/* An object with neither member is a server saying there is nothing
	 * to change, which is an answer and not a failure. */
	edit = read_edit("{}");
	CHECK(edit != NULL);
	if (edit) {
		CHECK(edit->item_count == 0);
		lsp_workspace_edit_free(edit);
	}
	{
		const char *bad = "{\"start\":{\"line\":-1,\"character\":0},"
				  "\"end\":{\"line\":0,\"character\":0}}";

		doc = kg_json_parse(bad, strlen(bad), NULL);
	}
	CHECK(doc != NULL);
	if (doc) {
		CHECK(!lsp_edit_range_read(kg_json_root(doc), &range));
		kg_json_free(doc);
	}
}

/* ------------------------------ the ordering -------------------------- */

static struct lsp_edit_span span(size_t begin, size_t end, const char *text)
{
	return (struct lsp_edit_span) { .begin = begin,
		.end = end,
		.text = text,
		.len = strlen(text),
		.order = 0 };
}

/* The same, saying where in the server's array the span came from, which
 * is what orders two of them at the same position. */
static struct lsp_edit_span span_nth(
    size_t begin, size_t end, const char *text, size_t order)
{
	struct lsp_edit_span out = span(begin, end, text);

	out.order = order;
	return out;
}

/* Ordered last edit first, so that every application is measured against
 * text no earlier edit has moved. */
static void test_spans_order_descending(void)
{
	struct lsp_edit_span spans[3]
	    = { span(4, 6, "b"), span(20, 24, "c"), span(0, 2, "a") };

	CHECK(lsp_edit_spans_order(spans, 3));
	CHECK(spans[0].begin == 20);
	CHECK(spans[1].begin == 4);
	CHECK(spans[2].begin == 0);
}

static void test_spans_reject_overlap(void)
{
	struct lsp_edit_span overlapping[2]
	    = { span(0, 5, "a"), span(3, 8, "b") };
	struct lsp_edit_span same[2] = { span(2, 4, "a"), span(2, 4, "b") };
	struct lsp_edit_span touching[2] = { span(0, 4, "a"), span(4, 8, "b") };
	struct lsp_edit_span inserts[2] = { span(4, 4, "a"), span(4, 4, "b") };

	CHECK(!lsp_edit_spans_order(overlapping, 2));
	CHECK(!lsp_edit_spans_order(same, 2));
	/* Adjacent is not overlapping, and neither are two insertions at
	 * one position: both describe a result. */
	CHECK(lsp_edit_spans_order(touching, 2));
	CHECK(lsp_edit_spans_order(inserts, 2));
}

/* Two insertions at one position are ordered by the array the server
 * sent, which is what the specification says decides.  qsort() is not
 * stable, so without the tiebreak this is "A then B" and "B then A"
 * producing whichever of them the sort happened to leave first. */
static void test_equal_spans_keep_the_array_order(void)
{
	struct lsp_edit_span inserts[2]
	    = { span_nth(4, 4, "A", 0), span_nth(4, 4, "B", 1) };
	size_t len = 0;
	char *out;

	CHECK(lsp_edit_spans_order(inserts, 2));
	/* Descending, and the array is consumed from the end: the edit the
	 * server sent first is the one written first. */
	CHECK(strcmp(inserts[0].text, "B") == 0);
	CHECK(strcmp(inserts[1].text, "A") == 0);
	out = lsp_edit_splice("0123456789", 10, 0, inserts, 2, &len);
	CHECK(out != NULL);
	if (out) {
		CHECKF(strcmp(out, "0123AB456789") == 0, "got '%s'", out);
		free(out);
	}
}

/* ------------------------------- the splice --------------------------- */

static void test_splice_applies_every_span(void)
{
	struct lsp_edit_span spans[2] = { span(8, 11, "ZZ"), span(0, 3, "Y") };
	const char *base = "abc def ghi";
	size_t len = 0;
	char *out;

	CHECK(lsp_edit_spans_order(spans, 2));
	out = lsp_edit_splice(base, strlen(base), 0, spans, 2, &len);
	CHECK(out != NULL);
	if (out) {
		CHECKF(strcmp(out, "Y def ZZ") == 0, "got '%s'", out);
		CHECK(len == strlen("Y def ZZ"));
		free(out);
	}
}

/* The base need not start at position zero: it is the document's own
 * bytes for the span the edits lie in, which is what the application
 * hands over. */
static void test_splice_is_offset_by_its_base(void)
{
	struct lsp_edit_span spans[1] = { span(12, 15, "XYZ") };
	const char *base = "abcdef";
	size_t len = 0;
	char *out = lsp_edit_splice(base, 6, 10, spans, 1, &len);

	CHECK(out != NULL);
	if (out) {
		CHECKF(strcmp(out, "abXYZf") == 0, "got '%s'", out);
		free(out);
	}
}

static void test_splice_refuses_a_span_outside_the_base(void)
{
	struct lsp_edit_span spans[1] = { span(2, 99, "x") };
	size_t len = 0;

	CHECK(lsp_edit_splice("abcdef", 6, 0, spans, 1, &len) == NULL);
}

/* ---------------------------- the conversion -------------------------- */

/* A character is not a column until it has been decoded against the
 * bytes of its row: the same range means two different byte spans under
 * the two encodings, and getting that wrong is a rename that eats a
 * glyph. */
static void test_range_span_decodes_the_encoding(void)
{
	struct lsp_edit_range range = { 0, 2, 0, 3 };
	struct lsp_edit_span out = { 0 };

	fixture("héllo");
	CHECK(lsp_edit_range_span(bcur(), LSP_POSITION_UTF16, &range, &out));
	/* "h" is one unit, "é" is one UTF-16 unit and two bytes, so units
	 * 2..3 are the byte range 3..4 -- the "l". */
	CHECK(out.begin == 3);
	CHECK(out.end == 4);
	CHECK(lsp_edit_range_span(bcur(), LSP_POSITION_UTF8, &range, &out));
	CHECK(out.begin == 2);
	CHECK(out.end == 3);
	teardown();
}

/* A line at or past the end of the buffer is a document kg does not
 * have, and is refused rather than clamped: clamping it appends the
 * newText at the end of the file and calls that a rename. */
static void test_range_span_refuses_a_line_past_the_end(void)
{
	struct lsp_edit_range past = { 900, 0, 900, 5 };
	struct lsp_edit_range last = { 2, 0, 2, 1 };
	struct lsp_edit_span out = { 0 };

	fixture("abc\ndef");
	CHECK(!lsp_edit_range_span(bcur(), LSP_POSITION_UTF8, &past, &out));
	CHECK(!lsp_edit_range_span(bcur(), LSP_POSITION_UTF8, &last, &out));
	/* The one position past the last row that does exist: the end of
	 * the document, which is how a server names the deletion of the
	 * final line. */
	{
		struct lsp_edit_range eof = { 1, 3, 2, 0 };

		CHECK(
		    lsp_edit_range_span(bcur(), LSP_POSITION_UTF8, &eof, &out));
		CHECK(out.end == buffer_byte_length(bcur()));
	}
	teardown();
}

/* The same for a character past the end of its row, in both encodings:
 * the clamp used to turn "replace columns 900..950" into "append here". */
static void test_range_span_refuses_a_character_past_the_row(void)
{
	struct lsp_edit_range past = { 0, 900, 0, 950 };
	struct lsp_edit_range end = { 0, 3, 0, 3 };
	struct lsp_edit_span out = { 0 };

	fixture("abc\ndef");
	CHECK(!lsp_edit_range_span(bcur(), LSP_POSITION_UTF8, &past, &out));
	CHECK(!lsp_edit_range_span(bcur(), LSP_POSITION_UTF16, &past, &out));
	/* The end of the row itself is inside it: that is where an
	 * insertion at the end of a line goes. */
	CHECK(lsp_edit_range_span(bcur(), LSP_POSITION_UTF8, &end, &out));
	CHECK(out.begin == 3 && out.end == 3);
	teardown();
}

/* One glyph, two units: a character past the end of a multibyte row is
 * measured in the encoding the handshake settled, not in bytes. */
static void test_range_span_measures_the_row_in_the_encoding(void)
{
	struct lsp_edit_range inside = { 0, 0, 0, 2 };
	struct lsp_edit_span out = { 0 };

	fixture("h\xc3\xa9"); /* "hé": 3 bytes, 2 UTF-16 units */
	CHECK(lsp_edit_range_span(bcur(), LSP_POSITION_UTF16, &inside, &out));
	CHECK(out.end == 3);
	{
		struct lsp_edit_range past = { 0, 0, 0, 3 };

		CHECK(!lsp_edit_range_span(
		    bcur(), LSP_POSITION_UTF16, &past, &out));
		/* The same numbers are bytes under utf-8, where 3 is the whole
		 * row and 4 is past it. */
		CHECK(lsp_edit_range_span(
		    bcur(), LSP_POSITION_UTF8, &past, &out));
	}
	teardown();
}

/* --------------------------- the application -------------------------- */

static struct lsp_workspace_edit *rename_edit(void)
{
	return read_edit(
	    "{\"changes\":{\"" FIXTURE_URI "\":["
	    "{\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{"
	    "\"line\":0,\"character\":10}},\"newText\":\"renamed\"},"
	    "{\"range\":{\"start\":{\"line\":2,\"character\":23},\"end\":{"
	    "\"line\":2,\"character\":29}},\"newText\":\"renamed\"}]}}");
}

static const char *rename_source(void)
{
	return "int target(void) { return 1; }\n"
	       "int other(void) { return 2; }\n"
	       "int use(void) { return target(); }";
}

/* Two occurrences, three lines apart, applied in one go -- and undone in
 * one go, which is the property the single-replacement application
 * exists for: kg's undo has no group bracket, so a record per occurrence
 * would be one C-_ per occurrence. */
static void test_apply_renames_every_occurrence_atomically(void)
{
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture(rename_source());
	edit = rename_edit();
	CHECK(edit != NULL);
	if (!edit) {
		teardown();
		return;
	}
	CHECK(lsp_workspace_edit_apply(
	    "lsp-rename", edit, LSP_POSITION_UTF8, NULL, &report));
	CHECK(report.edits == 2);
	CHECK(report.files == 1);
	CHECK(!report.refused);
	CHECK(!report.incomplete);
	CHECKF(strcmp(buffer_text(),
		   "int renamed(void) { return 1; }\n"
		   "int other(void) { return 2; }\n"
		   "int use(void) { return renamed(); }")
		== 0,
	    "got '%s'", buffer_text());
	CHECK(bcur()->dirty != 0);
	editor_undo();
	CHECKF(strcmp(buffer_text(), rename_source()) == 0, "got '%s'",
	    buffer_text());
	lsp_workspace_edit_free(edit);
	teardown();
}

/* The text between the occurrences is put back byte for byte, including
 * a line the edit never mentions -- the splice's whole job, seen from
 * the outside. */
static void test_apply_leaves_the_untouched_text_alone(void)
{
	struct lsp_workspace_edit *edit;

	fixture(rename_source());
	edit = rename_edit();
	if (edit) {
		CHECK(lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, NULL, NULL));
		CHECK(bcur()->numrows == 3);
		CHECK(strcmp(
			  bcur()->row[1].chars, "int other(void) { return 2; }")
		    == 0);
		lsp_workspace_edit_free(edit);
	}
	teardown();
}

/* Overlapping edits are refused for the whole file: applying the half
 * that does not overlap would leave a file neither side asked for. */
static void test_apply_refuses_overlapping_edits(void)
{
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture("abcdefghij");
	edit = read_edit(
	    "{\"changes\":{\"" FIXTURE_URI "\":["
	    "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
	    "\"line\":0,\"character\":5}},\"newText\":\"X\"},"
	    "{\"range\":{\"start\":{\"line\":0,\"character\":3},\"end\":{"
	    "\"line\":0,\"character\":8}},\"newText\":\"Y\"}]}}");
	CHECK(edit != NULL);
	if (edit) {
		CHECK(!lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, NULL, &report));
		CHECK(report.refused);
		CHECK(report.edits == 0);
		CHECKF(strcmp(buffer_text(), "abcdefghij") == 0, "got '%s'",
		    buffer_text());
		lsp_workspace_edit_free(edit);
	}
	teardown();
}

/* A read-only buffer refuses the edit the way it refuses any other: the
 * gateway says no, and the file is counted as refused rather than half
 * renamed. */
static void test_apply_refuses_a_read_only_buffer(void)
{
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture(rename_source());
	bcur()->readonly = 1;
	edit = rename_edit();
	if (edit) {
		CHECK(!lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, NULL, &report));
		CHECK(report.refused);
		CHECKF(strcmp(buffer_text(), rename_source()) == 0, "got '%s'",
		    buffer_text());
		lsp_workspace_edit_free(edit);
	}
	bcur()->readonly = 0;
	teardown();
}

/* An edit outside the file refuses the file, rather than landing at the
 * nearest position that does exist.  This is the review's repro A as a
 * unit test: a range on line 900 of a three-line buffer used to append
 * its newText at the end of the file and be reported as a rename. */
static void test_apply_refuses_an_edit_outside_the_file(void)
{
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture(rename_source());
	edit = read_edit("{\"changes\":{\"" FIXTURE_URI "\":["
			 "{\"range\":{\"start\":{\"line\":900,\"character\":0},"
			 "\"end\":{\"line\":900,\"character\":5}},\"newText\":"
			 "\"INJECTED\"}]}}");
	CHECK(edit != NULL);
	if (edit) {
		CHECK(!lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, NULL, &report));
		CHECK(report.refused);
		CHECK(report.edits == 0);
		CHECKF(strcmp(buffer_text(), rename_source()) == 0, "got '%s'",
		    buffer_text());
		lsp_workspace_edit_free(edit);
	}
	teardown();
}

/* The same for a character past the end of its row, which used to append
 * the newText to the line. */
static void test_apply_refuses_a_column_outside_the_row(void)
{
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture(rename_source());
	edit = read_edit("{\"changes\":{\"" FIXTURE_URI "\":["
			 "{\"range\":{\"start\":{\"line\":0,\"character\":900},"
			 "\"end\":{\"line\":0,\"character\":950}},\"newText\":"
			 "\"TAIL\"}]}}");
	CHECK(edit != NULL);
	if (edit) {
		CHECK(!lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, NULL, &report));
		CHECK(report.refused);
		CHECKF(strcmp(buffer_text(), rename_source()) == 0, "got '%s'",
		    buffer_text());
		lsp_workspace_edit_free(edit);
	}
	teardown();
}

/* The origin of the request, as the reply callback fills it: this buffer,
 * at the generation the question went out with. */
static struct lsp_edit_origin fixture_origin(uint64_t generation)
{
	struct lsp_edit_origin origin = { 0 };

	origin.buffer = buf_handle(buf_current);
	origin.generation = generation;
	origin.version = -1;
	origin.valid = true;
	return origin;
}

/* The CRITICAL one.  A buffer that moved between the question and the
 * answer is a buffer the answer is not about: the ranges name text that
 * has shifted, so applying them writes the newText somewhere nobody
 * asked.  Refused as a whole, with nothing written. */
static void test_apply_refuses_a_buffer_that_moved(void)
{
	struct lsp_edit_origin origin;
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture(rename_source());
	origin = fixture_origin(bcur()->content_generation + 1);
	edit = rename_edit();
	CHECK(edit != NULL);
	if (edit) {
		CHECK(!lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, &origin, &report));
		CHECK(report.refused);
		CHECK(report.edits == 0);
		CHECKF(strcmp(buffer_text(), rename_source()) == 0, "got '%s'",
		    buffer_text());
		/* The same answer against the generation it was asked at is
		 * applied, so the refusal is the staleness and not the check.
		 */
		origin = fixture_origin(bcur()->content_generation);
		CHECK(lsp_workspace_edit_apply(
		    "lsp-rename", edit, LSP_POSITION_UTF8, &origin, &report));
		CHECK(report.edits == 2);
		lsp_workspace_edit_free(edit);
	}
	teardown();
}

/* The same check spelled the protocol's way: a versioned identifier whose
 * version is not the one the server was last told is an edit about a
 * document that has moved, and eglot refuses exactly this. */
static void test_apply_refuses_a_version_mismatch(void)
{
	struct lsp_edit_origin origin;
	struct lsp_workspace_edit *edit;
	struct lsp_edit_report report = { 0 };

	fixture(rename_source());
	edit = read_edit(
	    "{\"documentChanges\":[{\"textDocument\":{\"uri\":\"" FIXTURE_URI
	    "\",\"version\":7},\"edits\":[{\"range\":{\"start\":{"
	    "\"line\":0,\"character\":4},\"end\":{\"line\":0,"
	    "\"character\":10}},\"newText\":\"renamed\"}]}]}");
	CHECK(edit != NULL);
	if (!edit) {
		teardown();
		return;
	}
	CHECK(edit->files[0].version == 7);
	origin = fixture_origin(bcur()->content_generation);
	origin.version = 3;
	CHECK(!lsp_workspace_edit_apply(
	    "lsp-rename", edit, LSP_POSITION_UTF8, &origin, &report));
	CHECK(report.refused);
	CHECKF(strcmp(buffer_text(), rename_source()) == 0, "got '%s'",
	    buffer_text());
	/* The version the server was told is the version it answered
	 * about: that one applies. */
	origin.version = 7;
	CHECK(lsp_workspace_edit_apply(
	    "lsp-rename", edit, LSP_POSITION_UTF8, &origin, &report));
	CHECK(report.edits == 1);
	lsp_workspace_edit_free(edit);
	teardown();
}

/* ---------------------------- completion logic ------------------------ */

static void test_common_prefix(void)
{
	const char *two[2] = { "alpha_one", "alpha_two" };
	const char *one[1] = { "alpha" };
	const char *none[2] = { "alpha", "beta" };

	CHECK(lsp_complete_common_prefix(two, 2) == 6); /* "alpha_" */
	CHECK(lsp_complete_common_prefix(one, 1) == 5);
	CHECK(lsp_complete_common_prefix(none, 2) == 0);
	CHECK(lsp_complete_common_prefix(NULL, 0) == 0);
}

/* Never half a character: two candidates sharing the first byte of a
 * two-byte glyph share nothing that can be inserted. */
static void test_common_prefix_keeps_utf8_whole(void)
{
	const char *split[2] = { "n\xc3\xa9"
				 "e",
		"n\xc3\xa8"
		"ud" };
	const char *whole[2] = { "n\xc3\xa9"
				 "e",
		"n\xc3\xa9"
		"on" };

	CHECK(lsp_complete_common_prefix(split, 2) == 1);
	CHECK(lsp_complete_common_prefix(whole, 2) == 3);
}

static void test_matches_is_a_prefix_test(void)
{
	CHECK(lsp_complete_matches("value_one", "val", 3));
	CHECK(!lsp_complete_matches("other", "val", 3));
	CHECK(!lsp_complete_matches("va", "val", 3));
	/* Case matters, as it does for kg's dabbrev. */
	CHECK(!lsp_complete_matches("Value", "val", 3));
	/* Nothing typed matches everything. */
	CHECK(lsp_complete_matches("anything", "", 0));
	CHECK(!lsp_complete_matches(NULL, "val", 3));
}

/* sortText first, label second: the protocol's own rule, and the reason
 * a server can put "the one you want" at the top of a listing. */
static void test_sort_is_by_sort_text_then_label(void)
{
	char labels[3][8] = { "beta", "alpha", "gamma" };
	char keys[3][8] = { "00", "99", "00" };
	struct lsp_complete_item items[3];
	size_t i;

	for (i = 0; i < 3; i++) {
		items[i] = (struct lsp_complete_item) { 0 };
		items[i].label = labels[i];
		items[i].sort_key = keys[i];
	}
	lsp_complete_sort(items, 3);
	CHECK(strcmp(items[0].label, "beta") == 0);
	CHECK(strcmp(items[1].label, "gamma") == 0);
	CHECK(strcmp(items[2].label, "alpha") == 0);
}

int main(void)
{
	RUN(test_reads_the_changes_shape);
	RUN(test_reads_the_document_changes_shape);
	RUN(test_an_absent_version_is_not_a_version);
	RUN(test_an_empty_edit_array_is_not_a_file);
	RUN(test_document_changes_win_over_changes);
	RUN(test_resource_operations_are_counted_not_applied);
	RUN(test_unreadable_uris_are_dropped);
	RUN(test_reading_refusals);
	RUN(test_spans_order_descending);
	RUN(test_spans_reject_overlap);
	RUN(test_equal_spans_keep_the_array_order);
	RUN(test_splice_applies_every_span);
	RUN(test_splice_is_offset_by_its_base);
	RUN(test_splice_refuses_a_span_outside_the_base);
	RUN(test_range_span_decodes_the_encoding);
	RUN(test_range_span_refuses_a_line_past_the_end);
	RUN(test_range_span_refuses_a_character_past_the_row);
	RUN(test_range_span_measures_the_row_in_the_encoding);
	RUN(test_apply_renames_every_occurrence_atomically);
	RUN(test_apply_leaves_the_untouched_text_alone);
	RUN(test_apply_refuses_overlapping_edits);
	RUN(test_apply_refuses_a_read_only_buffer);
	RUN(test_apply_refuses_an_edit_outside_the_file);
	RUN(test_apply_refuses_a_column_outside_the_row);
	RUN(test_apply_refuses_a_buffer_that_moved);
	RUN(test_apply_refuses_a_version_mismatch);
	RUN(test_common_prefix);
	RUN(test_common_prefix_keeps_utf8_whole);
	RUN(test_matches_is_a_prefix_test);
	RUN(test_sort_is_by_sort_text_then_label);
	return test_summary();
}
