/* test_showparen.c — show-paren-mode's matcher, and the seam that paints it
 *
 * show_paren_compute() is pure over a row array, so every rule the mode
 * has is asserted here rather than through a terminal: which position
 * triggers, which way the scan runs, how nesting is counted, when a pair
 * is a mismatch, which parens the syntactic class rule skips, and where
 * the scan bound cuts the search off.  Each expectation below was measured
 * against `emacs -q -nw` 31.0.90 (show-paren--default with the stock
 * defaults) before it was written down; the divergence the class rule
 * deliberately keeps is documented in src/showparen.c.
 *
 * What is NOT here: the colours.  A decoration's face is asserted at the
 * seam (test_update_publishes_a_face below); which SGR number that face
 * draws with is src/syntax.c's editor_syntax_to_color(), and whether the
 * terminal shows it is nothing any harness in this tree can read back --
 * tmux capture-pane gives the PTY suite text, not attributes.  The PTY
 * cases next door assert the other half: that having the highlight on
 * changes no byte of the file.
 */

#include "../src/decor.h"
#include "../src/def.h"
#include "../src/marker.h"
#include "../src/showparen.h"
#include "../src/syntax.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- A highlighter the test drives directly ----
 *
 * The syntactic class rule reads row->hl, and which bytes a real mode
 * paints is that backend's business (and differs between the legacy
 * scanners and tree-sitter).  So these tests supply their own mode-owned
 * highlighter and say, per row, exactly which bytes are string ('s'),
 * comment ('#') or code (anything else) -- the same arrangement
 * test_syntax.c uses to assert a colour without naming a backend. */
#define MAP_ROWS 8
static const char *row_class_map[MAP_ROWS];

static void map_highlight(struct editor_buffer *b, struct erow *row)
{
	const char *m;
	int i;

	(void)b;
	if (row->idx < 0 || row->idx >= MAP_ROWS) {
		return;
	}
	m = row_class_map[row->idx];
	for (i = 0; m && i < row->rsize && m[i]; i++) {
		if (m[i] == 's') {
			row->hl[i] = HL_STRING;
		} else if (m[i] == '#') {
			row->hl[i] = HL_COMMENT;
		}
	}
}

static struct editor_syntax mapped_mode = {
	.id = KG_MODE_TEXT,
	.name = (char *)"Mapped",
	.highlight = map_highlight,
};

static void setup(void)
{
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	reset_current_buffer();
	reset_current_view();
	bcur()->active = 1;
	memset(&editor, 0, sizeof(editor));
	memset(row_class_map, 0, sizeof(row_class_map));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	show_paren_mode = 1;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	show_paren_toggle(); /* off: drops whatever the seam published */
	show_paren_mode = 1;
	undo_free();
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

static void fill(const char *const *lines, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		editor_insert_row(bcur(), i, lines[i], (int)strlen(lines[i]));
	}
	bcur()->dirty = 0;
}

static void fill_one(const char *line)
{
	const char *lines[1] = { line };

	fill(lines, 1);
}

/* The whole answer for point (row, col), as one call. */
static struct show_paren_result at(int row, int col)
{
	struct show_paren_result res;

	show_paren_compute(bcur()->row, bcur()->numrows, row, col, &res);
	return res;
}

static int is_pair(struct show_paren_result r, enum show_paren_status status,
    int here_row, int here_col, int there_row, int there_col)
{
	return r.status == status && r.here_row == here_row
	    && r.here_col == here_col && r.there_row == there_row
	    && r.there_col == there_col;
}

/* ---- Trigger positions ---- */

/* Emacs highlights nothing when point is at neither end of a paren, and
 * -- show-paren-when-point-inside-paren being nil -- nothing when point is
 * just inside one either. */
static void test_only_the_two_outside_positions_trigger(void)
{
	setup();
	fill_one("(a)");

	CHECK(at(0, 1).status == SHOW_PAREN_NONE);
	CHECK(at(0, 2).status == SHOW_PAREN_NONE);
	CHECK(at(0, 0).status == SHOW_PAREN_MATCH);
	CHECK(at(0, 3).status == SHOW_PAREN_MATCH);
	teardown();
}

static void test_point_on_an_opener_scans_forward(void)
{
	setup();
	fill_one("(a)");

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 0, 2));
	teardown();
}

static void test_point_after_a_closer_scans_backward(void)
{
	setup();
	fill_one("(a)");

	CHECK(is_pair(at(0, 3), SHOW_PAREN_MATCH, 0, 2, 0, 0));
	teardown();
}

/* `)|(`: both rules could fire, and Emacs takes the closer before point.
 * Measured, not assumed: emacs -Q, "(a)(b)", point at offset 3 reports the
 * `)` at 2 paired with the `(` at 0. */
static void test_closer_before_point_beats_opener_after(void)
{
	setup();
	fill_one("(a)(b)");

	CHECK(is_pair(at(0, 3), SHOW_PAREN_MATCH, 0, 2, 0, 0));
	teardown();
}

static void test_nesting_pairs_the_right_depth(void)
{
	setup();
	fill_one("((a))");

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 0, 4));
	CHECK(is_pair(at(0, 1), SHOW_PAREN_MATCH, 0, 1, 0, 3));
	CHECK(is_pair(at(0, 4), SHOW_PAREN_MATCH, 0, 3, 0, 1));
	CHECK(is_pair(at(0, 5), SHOW_PAREN_MATCH, 0, 4, 0, 0));
	teardown();
}

static void test_pairs_span_rows(void)
{
	static const char *const lines[] = { "int f(int a,", "    int b)", "" };

	setup();
	fill(lines, 3);

	CHECK(is_pair(at(0, 5), SHOW_PAREN_MATCH, 0, 5, 1, 9));
	CHECK(is_pair(at(1, 10), SHOW_PAREN_MATCH, 1, 9, 0, 5));
	teardown();
}

/* ---- Kinds ---- */

static void test_all_three_kinds_pair(void)
{
	setup();
	fill_one("[a]");
	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 0, 2));
	teardown();

	setup();
	fill_one("{a}");
	CHECK(is_pair(at(0, 3), SHOW_PAREN_MATCH, 0, 2, 0, 0));
	teardown();
}

/* Nesting is counted across kinds and the kind is checked only once the
 * partner is found, so `(` closed by `]` is a found pair reported as a
 * mismatch -- which is exactly what Emacs reports for "(a]". */
static void test_wrong_kind_is_a_mismatch_not_a_miss(void)
{
	setup();
	fill_one("(a]");

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MISMATCH, 0, 0, 0, 2));
	CHECK(is_pair(at(0, 3), SHOW_PAREN_MISMATCH, 0, 2, 0, 0));
	teardown();
}

/* "([)]": counting ignores kinds, so `(` pairs with `]` and `[` with `)`,
 * both mismatches.  Emacs agrees, from all four positions. */
static void test_interleaved_kinds_pair_by_depth(void)
{
	setup();
	fill_one("([)]");

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MISMATCH, 0, 0, 0, 3));
	CHECK(is_pair(at(0, 1), SHOW_PAREN_MISMATCH, 0, 1, 0, 2));
	CHECK(is_pair(at(0, 3), SHOW_PAREN_MISMATCH, 0, 2, 0, 1));
	CHECK(is_pair(at(0, 4), SHOW_PAREN_MISMATCH, 0, 3, 0, 0));
	teardown();
}

/* A paren with no partner is the paren at point alone, in the mismatch
 * face -- Emacs' (HERE-BEG HERE-END nil nil t). */
static void test_unmatched_reports_only_the_paren_at_point(void)
{
	setup();
	fill_one("a)b");
	CHECK(at(0, 2).status == SHOW_PAREN_UNMATCHED);
	CHECK(at(0, 2).here_row == 0 && at(0, 2).here_col == 1);
	teardown();

	setup();
	fill_one("a(b");
	CHECK(at(0, 1).status == SHOW_PAREN_UNMATCHED);
	CHECK(at(0, 1).here_col == 1);
	teardown();
}

/* ---- The syntactic class rule ---- */

/* Parens inside one string pair with each other, a paren in a string never
 * reaches one outside it, and code skips a paren that is inside a string.
 * All three measured in emacs -Q, c-mode. */
static void test_string_parens_pair_only_with_string_parens(void)
{
	setup();
	/*                 x = " ( a ) " ; f ( 1 ) ; */
	row_class_map[0] = "cccssssscccccccc";
	bcur()->syntax = &mapped_mode;
	fill_one("x=\"(a)\";f(1);");

	/* Inside the string, with each other. */
	CHECK(is_pair(at(0, 3), SHOW_PAREN_MATCH, 0, 3, 0, 5));
	CHECK(is_pair(at(0, 6), SHOW_PAREN_MATCH, 0, 5, 0, 3));
	/* Outside it, with each other, skipping nothing they should not. */
	CHECK(is_pair(at(0, 9), SHOW_PAREN_MATCH, 0, 9, 0, 11));
	teardown();
}

static void test_a_string_paren_never_reaches_out_of_the_string(void)
{
	setup();
	/*                 " ( "   ) */
	row_class_map[0] = "sssccc";
	bcur()->syntax = &mapped_mode;
	fill_one("\"(\" )");

	CHECK(at(0, 1).status == SHOW_PAREN_UNMATCHED);
	CHECK(at(0, 5).status == SHOW_PAREN_UNMATCHED);
	teardown();
}

static void test_code_skips_a_paren_inside_a_string(void)
{
	setup();
	/*                 (   " ) "   ) */
	row_class_map[0] = "ccsssccc";
	bcur()->syntax = &mapped_mode;
	fill_one("( \")\" )");

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 0, 6));
	teardown();
}

static void test_comment_parens_are_their_own_partition(void)
{
	setup();
	/*                 / * space ( a )  space * /  space f ( 1 ) ; */
	row_class_map[0] = "##########ccccccc";
	bcur()->syntax = &mapped_mode;
	fill_one("/* (a) */ f(1);");

	/* Inside the comment, with each other. */
	CHECK(is_pair(at(0, 3), SHOW_PAREN_MATCH, 0, 3, 0, 5));
	/* And code below it never sees them. */
	CHECK(is_pair(at(0, 11), SHOW_PAREN_MATCH, 0, 11, 0, 13));
	teardown();
}

/* A buffer with no syntax at all has every byte HL_NORMAL, so every paren
 * is code and the quotes around one mean nothing.  That is plain-text
 * behaviour, and the one place kg's rule is knowingly looser than a
 * language-aware editor's. */
static void test_without_syntax_every_paren_is_code(void)
{
	setup();
	bcur()->syntax = NULL;
	fill_one("\"(\" )");

	CHECK(is_pair(at(0, 1), SHOW_PAREN_MATCH, 0, 1, 0, 4));
	teardown();
}

/* ---- Coordinates ---- */

/* row->hl is indexed by render bytes and the answer is in chars, so a row
 * whose tabs and multi-byte glyphs make the two spaces disagree is the
 * only row that proves the conversion at all (doc/coordinates.md). */
static void test_tabs_and_wide_glyphs_keep_the_answer_in_chars(void)
{
	setup();
	/* chars:  \t ( ä ) -> cols 0..3; render: 8 spaces, then ( ä ) */
	fill_one("\t(\xc3\xa4)");

	CHECK(is_pair(at(0, 1), SHOW_PAREN_MATCH, 0, 1, 0, 4));
	CHECK(is_pair(at(0, 5), SHOW_PAREN_MATCH, 0, 4, 0, 1));
	teardown();
}

/* ---- Edges ---- */

static void test_edges_do_not_reach_outside_the_buffer(void)
{
	struct show_paren_result res;

	setup();
	show_paren_compute(NULL, 0, 0, 0, &res);
	CHECK(res.status == SHOW_PAREN_NONE);
	show_paren_compute(bcur()->row, 0, 0, 0, &res);
	CHECK(res.status == SHOW_PAREN_NONE);

	fill_one("()");
	CHECK(at(-1, 0).status == SHOW_PAREN_NONE);
	CHECK(at(9, 0).status == SHOW_PAREN_NONE);
	/* A column past the end of the row clamps to the end, which is the
	 * after-the-closer position. */
	CHECK(is_pair(at(0, 99), SHOW_PAREN_MATCH, 0, 1, 0, 0));
	CHECK(is_pair(at(0, -1), SHOW_PAREN_MATCH, 0, 0, 0, 1));
	teardown();
}

static void test_empty_rows_are_walked_through(void)
{
	static const char *const lines[] = { "(", "", "", ")" };

	setup();
	fill(lines, 4);

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 3, 0));
	CHECK(is_pair(at(3, 1), SHOW_PAREN_MATCH, 3, 0, 0, 0));
	teardown();
}

static void test_deep_nesting_does_not_recurse(void)
{
	enum { DEPTH = 5000 };
	/* Static rather than malloc'd: the row is 10001 bytes and this is a
	 * test, so there is no allocation failure worth a branch here (and
	 * -fanalyzer is right to insist on one otherwise). */
	static char line[2 * DEPTH + 1];
	int i;

	setup();
	for (i = 0; i < DEPTH; i++) {
		line[i] = '(';
		line[2 * DEPTH - 1 - i] = ')';
	}
	line[2 * DEPTH] = '\0';
	fill_one(line);

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 0, 2 * DEPTH - 1));
	CHECK(is_pair(
	    at(0, DEPTH - 1), SHOW_PAREN_MATCH, 0, DEPTH - 1, 0, DEPTH));
	teardown();
}

/* ---- The scan bound ---- */

/* KG_SHOW_PAREN_MAX_SCAN is Emacs' blink-matching-paren-distance, and a
 * search that runs past it reports no partner rather than a wrong one --
 * which is also what Emacs does: measured, "(" + 102300 filler + ")"
 * highlights the pair and 102500 filler reports (HERE nil nil t). */
static void test_a_match_beyond_the_bound_is_no_match(void)
{
	enum { WIDTH = 600 };
	char filler[WIDTH + 1];
	int rows_needed = KG_SHOW_PAREN_MAX_SCAN / (WIDTH + 1) + 4;
	int i;

	memset(filler, 'x', WIDTH);
	filler[WIDTH] = '\0';

	setup();
	editor_insert_row(bcur(), 0, "(", 1);
	for (i = 0; i < rows_needed; i++) {
		editor_insert_row(bcur(), 1 + i, filler, WIDTH);
	}
	editor_insert_row(bcur(), 1 + rows_needed, ")", 1);

	CHECK(at(0, 0).status == SHOW_PAREN_UNMATCHED);
	CHECKF(at(0, 0).scanned <= KG_SHOW_PAREN_MAX_SCAN + 1,
	    "scanned %ld bytes, bound is %d", at(0, 0).scanned,
	    KG_SHOW_PAREN_MAX_SCAN);
	CHECK(at(1 + rows_needed, 1).status == SHOW_PAREN_UNMATCHED);
	teardown();
}

static void test_a_match_just_inside_the_bound_is_found(void)
{
	enum { WIDTH = 600 };
	char filler[WIDTH + 1];
	int rows_needed = KG_SHOW_PAREN_MAX_SCAN / (WIDTH + 1) - 4;
	int i;

	memset(filler, 'x', WIDTH);
	filler[WIDTH] = '\0';

	setup();
	editor_insert_row(bcur(), 0, "(", 1);
	for (i = 0; i < rows_needed; i++) {
		editor_insert_row(bcur(), 1 + i, filler, WIDTH);
	}
	editor_insert_row(bcur(), 1 + rows_needed, ")", 1);

	CHECK(is_pair(at(0, 0), SHOW_PAREN_MATCH, 0, 0, 1 + rows_needed, 0));
	teardown();
}

/* The performance property, asserted as a shape rather than a wall time:
 * the work one computation does is bounded by the DISTANCE to the partner,
 * not by the size of the buffer.  A counter would say the same thing less
 * directly -- `scanned` is already the number, and it is reported by an
 * ordinary build, so this needs no KG_PERF_COUNTERS lane. */
static void test_work_is_proportional_to_the_distance_not_the_buffer(void)
{
	enum { WIDTH = 600, ROWS = 200 };
	char filler[WIDTH + 1];
	int i;

	memset(filler, 'x', WIDTH);
	filler[WIDTH] = '\0';

	setup();
	editor_insert_row(bcur(), 0, "()", 2);
	for (i = 0; i < ROWS; i++) {
		editor_insert_row(bcur(), 1 + i, filler, WIDTH);
	}

	/* > 120000 bytes of buffer below; the pair at point is two bytes. */
	CHECK(at(0, 0).status == SHOW_PAREN_MATCH);
	CHECKF(at(0, 0).scanned <= 4, "scanned %ld", at(0, 0).scanned);
	/* And a paren with no partner pays the bound once, not the buffer:
	 * this buffer is smaller than the bound, so the whole of it. */
	CHECK(at(0, 1).status == SHOW_PAREN_NONE); /* inside the pair */
	teardown();
}

/* ---- The editor seam ---- */

static int decor_count(void)
{
	return bcur()->decorations ? (int)bcur()->decorations->count : 0;
}

static int decor_face_at(size_t pos, enum kg_decor_face *out)
{
	struct kg_decor_query q;
	struct kg_decor_query_span s;

	kg_decor_query_begin(&q, bcur(), pos, pos + 1);
	while (kg_decor_query_next(&q, &s)) {
		if (s.start == pos) {
			*out = s.face;
			return 1;
		}
	}
	return 0;
}

static void point_at(int row, int col)
{
	wcur()->rowoff = 0;
	wcur()->coloff = 0;
	wcur()->cy = row;
	wcur()->cx = col;
}

/* One decoration per highlighted paren, one byte wide, carrying the face
 * the status calls for -- and nothing left over from the position before. */
static void test_update_publishes_a_face(void)
{
	enum kg_decor_face face;

	setup();
	fill_one("(a)");

	point_at(0, 0);
	show_paren_update();
	CHECK(decor_count() == 2);
	CHECK(decor_face_at(0, &face) && face == KG_DECOR_FACE_PAREN_MATCH);
	CHECK(decor_face_at(2, &face) && face == KG_DECOR_FACE_PAREN_MATCH);

	/* Move off the paren: the highlight goes with the cursor. */
	point_at(0, 1);
	show_paren_update();
	CHECK(decor_count() == 0);
	teardown();
}

static void test_update_publishes_the_mismatch_face(void)
{
	enum kg_decor_face face;

	setup();
	fill_one("a)b");

	point_at(0, 2);
	show_paren_update();
	CHECK(decor_count() == 1);
	CHECK(decor_face_at(1, &face) && face == KG_DECOR_FACE_PAREN_MISMATCH);
	teardown();
}

/* The mode is on by default, as in Emacs 28.1 and later, and the toggle
 * both stops publishing and retires what is already published. */
static void test_toggle_is_on_by_default_and_clears_on_the_way_off(void)
{
	setup();
	CHECK(show_paren_mode == 1);
	fill_one("(a)");
	point_at(0, 0);
	show_paren_update();
	CHECK(decor_count() == 2);

	show_paren_toggle();
	CHECK(show_paren_mode == 0);
	CHECK(decor_count() == 0);
	show_paren_update();
	CHECK(decor_count() == 0);

	show_paren_toggle();
	CHECK(show_paren_mode == 1);
	show_paren_update();
	CHECK(decor_count() == 2);
	teardown();
}

/* A repaint that changes neither point nor text republishes nothing: the
 * decorations on screen are the same records, not new ones with new IDs. */
static void test_a_still_frame_republishes_nothing(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span s;
	uint32_t first_id = 0;

	setup();
	fill_one("(a)");
	point_at(0, 0);
	show_paren_update();
	kg_decor_query_begin(&q, bcur(), 0, 1);
	CHECK(kg_decor_query_next(&q, &s));
	first_id = s.id;

	show_paren_update();
	show_paren_update();
	kg_decor_query_begin(&q, bcur(), 0, 1);
	CHECK(kg_decor_query_next(&q, &s));
	CHECK(s.id == first_id);
	CHECK(decor_count() == 2);
	teardown();
}

int main(void)
{
	RUN(test_only_the_two_outside_positions_trigger);
	RUN(test_point_on_an_opener_scans_forward);
	RUN(test_point_after_a_closer_scans_backward);
	RUN(test_closer_before_point_beats_opener_after);
	RUN(test_nesting_pairs_the_right_depth);
	RUN(test_pairs_span_rows);
	RUN(test_all_three_kinds_pair);
	RUN(test_wrong_kind_is_a_mismatch_not_a_miss);
	RUN(test_interleaved_kinds_pair_by_depth);
	RUN(test_unmatched_reports_only_the_paren_at_point);
	RUN(test_string_parens_pair_only_with_string_parens);
	RUN(test_a_string_paren_never_reaches_out_of_the_string);
	RUN(test_code_skips_a_paren_inside_a_string);
	RUN(test_comment_parens_are_their_own_partition);
	RUN(test_without_syntax_every_paren_is_code);
	RUN(test_tabs_and_wide_glyphs_keep_the_answer_in_chars);
	RUN(test_edges_do_not_reach_outside_the_buffer);
	RUN(test_empty_rows_are_walked_through);
	RUN(test_deep_nesting_does_not_recurse);
	RUN(test_a_match_beyond_the_bound_is_no_match);
	RUN(test_a_match_just_inside_the_bound_is_found);
	RUN(test_work_is_proportional_to_the_distance_not_the_buffer);
	RUN(test_update_publishes_a_face);
	RUN(test_update_publishes_the_mismatch_face);
	RUN(test_toggle_is_on_by_default_and_clears_on_the_way_off);
	RUN(test_a_still_frame_republishes_nothing);
	return test_summary();
}
