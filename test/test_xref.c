/* test_xref.c — the location parser behind M-. (src/xref.c, Stage 5 of
 * doc/plans/2026-08-08-lsp.md).
 *
 * The command itself is PTY-only: it needs a server, a poll site and a
 * window to land point in, and a native test that faked all three would be
 * testing the fake.  What is native here is the part that decides *where*
 * a jump goes -- xref_location_of(), which turns one element of a
 * `textDocument/definition` reply into a path and a position.
 *
 * That parser is where the protocol's generosity has to be absorbed: the
 * same answer arrives as a Location or as a LocationLink, from clangd with
 * a selection range and from a server that only sends the target range, and
 * the wrong branch is not a crash -- it is a jump to the first line of a
 * forty-line function, or to a file kg never checked was a file.  So every
 * shape gets a case, and so does every refusal.
 *
 * This links the whole editor (Makefile's EXTRA_xref) because src/xref.o
 * does; nothing below touches a buffer.
 */

#include "../src/def.h"
#include "../src/json.h"
#include "../src/xref.h"
#include "test.h"

#include <string.h>

/* Parse a literal and hand xref_location_of() its root, so a case reads as
 * the JSON a server would send. */
static bool location_of(const char *json, struct xref_location *out)
{
	struct kg_json *doc = kg_json_parse(json, strlen(json), NULL);
	bool ok;

	if (!doc) {
		return false;
	}
	ok = xref_location_of(kg_json_root(doc), out);
	kg_json_free(doc);
	return ok;
}

/* The plain shape: what clangd answers a definition request with when it
 * answers with one Location rather than an array of them. */
static void test_location_shape(void)
{
	struct xref_location loc = { 0 };

	CHECK(location_of("{\"uri\":\"file:///tmp/a.c\","
			  "\"range\":{\"start\":{\"line\":12,\"character\":4},"
			  "\"end\":{\"line\":12,\"character\":9}}}",
	    &loc));
	CHECK(strcmp(loc.path, "/tmp/a.c") == 0);
	CHECK(loc.line == 12);
	CHECK(loc.character == 4);
}

/* A LocationLink names two ranges, and the selection one is the identifier
 * rather than the whole definition.  Landing on the body's first line when
 * the server said exactly where the name is would be the worse answer, so
 * the preference is asserted with both present and disagreeing. */
static void test_location_link_prefers_selection_range(void)
{
	struct xref_location loc = { 0 };

	CHECK(location_of("{\"targetUri\":\"file:///tmp/b.c\","
			  "\"targetRange\":{\"start\":{\"line\":3,"
			  "\"character\":0},\"end\":{\"line\":40,"
			  "\"character\":1}},"
			  "\"targetSelectionRange\":{\"start\":{\"line\":5,"
			  "\"character\":7},\"end\":{\"line\":5,"
			  "\"character\":11}}}",
	    &loc));
	CHECK(strcmp(loc.path, "/tmp/b.c") == 0);
	CHECK(loc.line == 5);
	CHECK(loc.character == 7);
}

/* And falls back to the target range when that is all there is. */
static void test_location_link_falls_back_to_target_range(void)
{
	struct xref_location loc = { 0 };

	CHECK(location_of("{\"targetUri\":\"file:///tmp/c.c\","
			  "\"targetRange\":{\"start\":{\"line\":3,"
			  "\"character\":2},\"end\":{\"line\":9,"
			  "\"character\":1}}}",
	    &loc));
	CHECK(strcmp(loc.path, "/tmp/c.c") == 0);
	CHECK(loc.line == 3);
	CHECK(loc.character == 2);
}

/* A percent-encoded path decodes to the bytes it names; that is
 * src/lsp_uri.c's job, and this asserts the parser actually routes through
 * it rather than slicing the URI itself. */
static void test_uri_is_decoded(void)
{
	struct xref_location loc = { 0 };

	CHECK(location_of("{\"uri\":\"file:///tmp/a%20b/x%2By.c\","
			  "\"range\":{\"start\":{\"line\":0,"
			  "\"character\":0}}}",
	    &loc));
	CHECK(strcmp(loc.path, "/tmp/a b/x+y.c") == 0);
}

/* Everything that must be refused rather than navigated to.  The last two
 * matter most: a scheme kg cannot open, and a negative line, are both
 * things a server can send and neither has a sensible clamp -- there is no
 * "nearest file" and no line before the first. */
static void test_refusals(void)
{
	struct xref_location loc = { 0 };

	CHECK(!location_of("null", &loc));
	CHECK(!location_of("[]", &loc));
	CHECK(!location_of("{}", &loc));
	/* No range at all. */
	CHECK(!location_of("{\"uri\":\"file:///tmp/a.c\"}", &loc));
	/* A range with no start. */
	CHECK(!location_of("{\"uri\":\"file:///tmp/a.c\",\"range\":{}}", &loc));
	/* A start with no line. */
	CHECK(!location_of("{\"uri\":\"file:///tmp/a.c\","
			   "\"range\":{\"start\":{\"character\":1}}}",
	    &loc));
	/* A scheme kg has no way to open. */
	CHECK(!location_of("{\"uri\":\"https://example.com/a.c\","
			   "\"range\":{\"start\":{\"line\":1,"
			   "\"character\":0}}}",
	    &loc));
	/* A line before the first. */
	CHECK(!location_of("{\"uri\":\"file:///tmp/a.c\","
			   "\"range\":{\"start\":{\"line\":-1,"
			   "\"character\":0}}}",
	    &loc));
}

/* A character the protocol should never send, clamped rather than refused:
 * the position is still a position, and a negative column decoded against a
 * row would index before it. */
static void test_negative_character_clamps(void)
{
	struct xref_location loc = { 0 };

	CHECK(location_of("{\"uri\":\"file:///tmp/a.c\","
			  "\"range\":{\"start\":{\"line\":2,"
			  "\"character\":-4}}}",
	    &loc));
	CHECK(loc.line == 2);
	CHECK(loc.character == 0);
}

/* Nothing has jumped, so nothing has been remembered.  The stack itself is
 * exercised end to end by the PTY cases; this is the empty-state half of
 * its contract. */
static void test_go_back_starts_empty(void)
{
	CHECK(xref_go_back_depth() == 0);
}

/* M-, with nothing to go back to says so and moves nothing.  Worth a native
 * case rather than a PTY one because it is the branch a PTY case cannot
 * assert: an editor that "went back" nowhere and an editor that reported an
 * empty history look identical in a saved file, and differ only in the echo
 * area. */
static void test_go_back_on_an_empty_stack_reports(void)
{
	editor.statusmsg[0] = '\0';
	editor_xref_go_back(0);
	CHECK(xref_go_back_depth() == 0);
	CHECKF(strcmp(editor.statusmsg, "No xref history") == 0, "got '%s'",
	    editor.statusmsg);
}

int main(void)
{
	RUN(test_location_shape);
	RUN(test_location_link_prefers_selection_range);
	RUN(test_location_link_falls_back_to_target_range);
	RUN(test_uri_is_decoded);
	RUN(test_refusals);
	RUN(test_negative_character_clamps);
	RUN(test_go_back_starts_empty);
	RUN(test_go_back_on_an_empty_stack_reports);
	return test_summary();
}
