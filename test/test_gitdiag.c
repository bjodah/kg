/* test_gitdiag.c — git-mode diagnostics: the two rules, and the seam
 *
 * These used to be assertions about row->hl in test_syntax_legacy.c,
 * which only a WITH_TREE_SITTER=0 tree builds -- and that was the bug:
 * the warnings were the legacy scanner's, so the other backend showed
 * none.  They are decorations now, computed by backend-independent code,
 * so this suite links no scanner and no parser and runs in both
 * configurations.
 *
 * The expectations are the legacy scanner's, deliberately unchanged: the
 * same 50-column subject limit measured over the rendered line, the same
 * action vocabulary, and the same "only fixup and merge take -C/-c" flag
 * rule.  What is asserted here is the span and the face; which SGR number
 * that face draws with is src/syntax.c's editor_syntax_to_color(), and no
 * harness in this tree can read a terminal attribute back (tmux
 * capture-pane gives the PTY suite text, not attributes).
 */

#include "../src/decor.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/gitdiag.h"
#include "../src/marker.h"
#include "../src/syntax.h"
#include "test.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setup(struct editor_syntax *mode)
{
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	reset_current_buffer();
	reset_current_view();
	bcur()->active = 1;
	bcur()->syntax = mode;
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	/* Retire whatever the seam published: the module keeps its handles
	 * in its own store, and the next test's buffer is this one. */
	bcur()->syntax = NULL;
	gitdiag_update(bcur());
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

/* One row of exactly `len` bytes, for the lines whose length is the
 * thing under test. */
static void fill_len(const char *text, int len)
{
	editor_insert_row(bcur(), 0, text, len);
	bcur()->dirty = 0;
}

/* ---- The subject-length rule (pure) ---- */

static int subject_span(int *start, int *end)
{
	struct kg_gitdiag_span span = { -1, -1 };
	int hit = gitdiag_commit_subject_span(
	    &bcur()->row[0], &bcur()->display, &span);

	*start = span.start;
	*end = span.end;
	return hit;
}

/* Exactly at the limit is not over it; one byte past is one byte of
 * warning, and the span always runs to the end of the line. */
static void test_subject_span_begins_at_the_limit(void)
{
	char line[KG_GITDIAG_SUBJECT_LIMIT + 10];
	int start = -1, end = -1;

	memset(line, 'x', sizeof(line));

	setup(syntax_find_by_name("Git commit"));
	fill_len(line, KG_GITDIAG_SUBJECT_LIMIT);
	CHECK(!subject_span(&start, &end));
	teardown();

	setup(syntax_find_by_name("Git commit"));
	fill_len(line, KG_GITDIAG_SUBJECT_LIMIT + 1);
	CHECK(subject_span(&start, &end));
	CHECK(start == KG_GITDIAG_SUBJECT_LIMIT);
	CHECK(end == KG_GITDIAG_SUBJECT_LIMIT + 1);
	teardown();

	setup(syntax_find_by_name("Git commit"));
	fill_len(line, KG_GITDIAG_SUBJECT_LIMIT + 10);
	CHECK(subject_span(&start, &end));
	CHECK(start == KG_GITDIAG_SUBJECT_LIMIT);
	CHECK(end == KG_GITDIAG_SUBJECT_LIMIT + 10);
	teardown();
}

/* The limit counts rendered columns -- where the legacy scanner counted
 * them -- so a TAB is worth its expansion; the span it produces is still
 * a chars-space one.  A leading TAB puts render byte 50 at chars offset
 * 1 + (50 - KG_TAB_WIDTH). */
static void test_subject_span_counts_rendered_columns(void)
{
	char line[64];
	int start = -1, end = -1;
	int expect = 1 + KG_GITDIAG_SUBJECT_LIMIT - KG_TAB_WIDTH;

	line[0] = '\t';
	memset(line + 1, 'x', 45);

	setup(syntax_find_by_name("Git commit"));
	fill_len(line, 46);
	CHECK(bcur()->row[0].rsize == KG_TAB_WIDTH + 45);
	CHECK(subject_span(&start, &end));
	CHECK(start == expect);
	CHECK(end == 46);
	teardown();
}

/* A line that is only over the limit because a TAB expands past it has no
 * character out there to mark, and reports nothing rather than an empty
 * span. */
static void test_subject_span_ignores_an_empty_overhang(void)
{
	char tabs[7];
	int start = -1, end = -1;

	memset(tabs, '\t', sizeof(tabs));

	setup(syntax_find_by_name("Git commit"));
	fill_len(tabs, (int)sizeof(tabs));
	CHECK(bcur()->row[0].rsize > KG_GITDIAG_SUBJECT_LIMIT);
	CHECK(!subject_span(&start, &end));
	teardown();
}

/* ---- The rebase-action rule (pure) ---- */

static int rebase_spans(const char *line, struct kg_gitdiag_span *out, int max)
{
	return gitdiag_rebase_row_spans(line, (int)strlen(line), out, max);
}

static int rebase_clean(const char *line)
{
	struct kg_gitdiag_span spans[KG_GITDIAG_ROW_SPANS_MAX];

	return rebase_spans(line, spans, KG_GITDIAG_ROW_SPANS_MAX) == 0;
}

/* Every line git accepts reports nothing: known actions long and
 * abbreviated, an exec body (whose own flags are the command's, not the
 * todo's), comments, blanks and whitespace. */
static void test_rebase_accepts_what_git_accepts(void)
{
	CHECK(rebase_clean("pick 1a2b3c4 a subject"));
	CHECK(rebase_clean("p 1a2b3c4 a subject"));
	CHECK(rebase_clean("reword 1a2b3c4 x"));
	CHECK(rebase_clean("fixup -C 1a2b3c4 x"));
	CHECK(rebase_clean("f -c 1a2b3c4 x"));
	CHECK(rebase_clean("merge -C 1a2b3c4 label"));
	CHECK(rebase_clean("exec make -j8 check"));
	CHECK(rebase_clean("break"));
	CHECK(rebase_clean("noop"));
	CHECK(rebase_clean("update-ref refs/heads/topic"));
	CHECK(rebase_clean("# Rebase abc..def onto abc"));
	CHECK(rebase_clean(""));
	CHECK(rebase_clean("   \t "));
}

/* A word that names no action fails the whole rebase, so it is one span
 * over exactly that word, wherever the indentation puts it. */
static void test_rebase_unknown_action_is_one_span(void)
{
	struct kg_gitdiag_span spans[KG_GITDIAG_ROW_SPANS_MAX];

	CHECK(rebase_spans("puck 1a2b3c4 oops", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 1);
	CHECK(spans[0].start == 0 && spans[0].end == 4);

	CHECK(rebase_spans("  \tpuck 1a2b3c4", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 1);
	CHECK(spans[0].start == 3 && spans[0].end == 7);
}

/* Only fixup and merge take -C/-c; anything else before the hash is a
 * flag git will refuse, and each one is its own span. */
static void test_rebase_flags_follow_the_action(void)
{
	struct kg_gitdiag_span spans[KG_GITDIAG_ROW_SPANS_MAX];

	CHECK(rebase_spans("pick -C 1a2b3c4 x", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 1);
	CHECK(spans[0].start == 5 && spans[0].end == 7);

	CHECK(rebase_spans("fixup -X 1a2b3c4", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 1);
	CHECK(spans[0].start == 6 && spans[0].end == 8);

	CHECK(
	    rebase_spans("fixup -C -X 1a2b3c4", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 1);
	CHECK(spans[0].start == 9 && spans[0].end == 11);

	CHECK(rebase_spans("pick - 1a2b3c4", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 1);
	CHECK(spans[0].start == 5 && spans[0].end == 6);

	CHECK(
	    rebase_spans("pick -a -b 1a2b3c4", spans, KG_GITDIAG_ROW_SPANS_MAX)
	    == 2);
	CHECK(spans[0].start == 5 && spans[0].end == 7);
	CHECK(spans[1].start == 8 && spans[1].end == 10);

	/* An action that takes neither a commit nor a flag is not read
	 * past its own word, exactly as the scanner did not read it. */
	CHECK(rebase_clean("label -C x"));
}

/* The bounded output is a cap, not a buffer overrun: the caller's `max`
 * is honoured and the spans it does get are the first ones. */
static void test_rebase_span_list_is_bounded(void)
{
	struct kg_gitdiag_span spans[3];

	spans[2].start = -1;
	CHECK(rebase_spans("pick -a -b -c -d 1a2b3c4", spans, 2) == 2);
	CHECK(spans[0].start == 5 && spans[1].start == 8);
	CHECK(spans[2].start == -1);
}

/* ---- The seam ---- */

static int decor_count(void)
{
	return bcur()->decorations ? (int)bcur()->decorations->count : 0;
}

/* The one decoration covering [start, end) of row `row`, with the warning
 * face; false when the store holds no such span. */
static int warned(int row, int start, int end)
{
	struct kg_decor_query q;
	struct kg_decor_query_span s;
	size_t from = buffer_row_col_to_position(bcur(), row, start);
	size_t to = buffer_row_col_to_position(bcur(), row, end);

	kg_decor_query_begin(&q, bcur(), 0, buffer_byte_length(bcur()) + 1);
	while (kg_decor_query_next(&q, &s)) {
		if (s.start == from && s.end == to
		    && s.face == KG_DECOR_FACE_WARNING) {
			return 1;
		}
	}
	return 0;
}

/* Replace the buffer bytes [begin, end) with `text`, as one edit. */
static void edit(size_t begin, size_t end, const char *text)
{
	struct kg_edit e = kg_edit_user(bcur(), begin, end, text, strlen(text));

	CHECK(kg_buffer_replace(&e, NULL) == 1);
}

/* The subject is the first non-blank, non-comment row, so a commit
 * template's leading comments and blank lines are walked through -- and
 * the body below it is not a subject and is never marked. */
static void test_update_marks_the_subject_and_nothing_else(void)
{
	char body[71];
	const char *lines[5] = { "# Please enter the commit message", "",
		"a subject line that is a good deal longer than fifty bytes",
		"", body };

	memset(body, 'y', sizeof(body) - 1);
	body[sizeof(body) - 1] = '\0';

	setup(syntax_find_by_name("Git commit"));
	fill(lines, 5);
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);
	CHECK(warned(2, KG_GITDIAG_SUBJECT_LIMIT, (int)strlen(lines[2])));
	teardown();
}

/* The edited line's diagnosis is refreshed: shortening the subject
 * retires the warning, and lengthening it again brings it back. */
static void test_update_follows_the_edit(void)
{
	const char *subject
	    = "a subject line that is a good deal longer than fifty bytes";
	size_t len = strlen(subject);

	setup(syntax_find_by_name("Git commit"));
	fill_one(subject);
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);

	edit(KG_GITDIAG_SUBJECT_LIMIT, len, "");
	gitdiag_update(bcur());
	CHECK(decor_count() == 0);

	edit(KG_GITDIAG_SUBJECT_LIMIT, KG_GITDIAG_SUBJECT_LIMIT, "again");
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);
	CHECK(
	    warned(0, KG_GITDIAG_SUBJECT_LIMIT, KG_GITDIAG_SUBJECT_LIMIT + 5));
	teardown();
}

/* Every diagnosable line of a todo list gets its own span, and the clean
 * ones get none. */
static void test_update_marks_every_bad_rebase_line(void)
{
	const char *lines[4] = { "pick 1a2b3c4 fine", "puck 2b3c4d5 typo",
		"pick -C 3c4d5e6 flag", "exec make check" };

	setup(syntax_find_by_name("Git rebase"));
	fill(lines, 4);
	gitdiag_update(bcur());
	CHECK(decor_count() == 2);
	CHECK(warned(1, 0, 4));
	CHECK(warned(2, 5, 7));
	teardown();
}

/* A decoration is marker-anchored, so an edit somewhere else moves it
 * with its text before anything recomputes -- and the recomputation then
 * agrees with where it moved to. */
static void test_a_published_span_relocates_with_the_text(void)
{
	const char *lines[2] = { "pick 1a2b3c4 fine", "puck 2b3c4d5 typo" };

	setup(syntax_find_by_name("Git rebase"));
	fill(lines, 2);
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);
	CHECK(warned(1, 0, 4));

	/* A whole new first line, inserted above both: no recomputation
	 * yet, and the span already names the same word on its new row. */
	edit(0, 0, "pick 0a1b2c3 first\n");
	CHECK(decor_count() == 1);
	CHECK(warned(2, 0, 4));

	gitdiag_update(bcur());
	CHECK(decor_count() == 1);
	CHECK(warned(2, 0, 4));
	teardown();
}

/* A buffer that is not in a git mode has no diagnostics, and leaving a
 * git mode retires the ones it had. */
static void test_update_retires_on_a_mode_change(void)
{
	const char *subject
	    = "a subject line that is a good deal longer than fifty bytes";

	setup(syntax_find_by_name("Git commit"));
	fill_one(subject);
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);

	bcur()->syntax = syntax_find_by_name("C");
	gitdiag_update(bcur());
	CHECK(decor_count() == 0);

	bcur()->syntax = syntax_find_by_name("Git commit");
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);
	teardown();
}

/* A frame that changed nothing republishes nothing: the same decoration
 * is still there, with the same identity, rather than a fresh one over
 * the same bytes. */
static void test_a_still_frame_republishes_nothing(void)
{
	struct kg_decor_query q;
	struct kg_decor_query_span s;
	uint32_t first_id;

	setup(syntax_find_by_name("Git commit"));
	fill_one("a subject line that is a good deal longer than fifty bytes");
	gitdiag_update(bcur());
	kg_decor_query_begin(&q, bcur(), 0, buffer_byte_length(bcur()) + 1);
	CHECK(kg_decor_query_next(&q, &s));
	first_id = s.id;

	gitdiag_update(bcur());
	gitdiag_update(bcur());
	CHECK(decor_count() == 1);
	kg_decor_query_begin(&q, bcur(), 0, buffer_byte_length(bcur()) + 1);
	CHECK(kg_decor_query_next(&q, &s));
	CHECK(s.id == first_id);
	teardown();
}

int main(void)
{
	RUN(test_subject_span_begins_at_the_limit);
	RUN(test_subject_span_counts_rendered_columns);
	RUN(test_subject_span_ignores_an_empty_overhang);
	RUN(test_rebase_accepts_what_git_accepts);
	RUN(test_rebase_unknown_action_is_one_span);
	RUN(test_rebase_flags_follow_the_action);
	RUN(test_rebase_span_list_is_bounded);
	RUN(test_update_marks_the_subject_and_nothing_else);
	RUN(test_update_follows_the_edit);
	RUN(test_update_marks_every_bad_rebase_line);
	RUN(test_a_published_span_relocates_with_the_text);
	RUN(test_update_retires_on_a_mode_change);
	RUN(test_a_still_frame_republishes_nothing);
	return test_summary();
}
