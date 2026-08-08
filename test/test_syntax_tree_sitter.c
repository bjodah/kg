/* test_syntax_tree_sitter.c -- regression tests for the tree-sitter backend
 *
 * Everything src/syntax_tree_sitter.c and src/syntax_tree_sitter_lang.c
 * decide: that a grammar loads, that kg's own highlight query compiles
 * against it, that a capture becomes the right HL_* face at the right
 * offset of row->render, and that a buffer's parser and tree come and go
 * with the mode.  A build that installs a different backend does not build
 * this suite (the Makefile adds it to TESTBINS only when
 * WITH_TREE_SITTER=1); the backend-neutral half -- mode identity,
 * selection, colours, syntax-state lifetime -- is test/test_syntax.c, and
 * the other backend's own assertions are test/test_syntax_legacy.c.
 *
 * The C grammar is hard-required rather than skipped around.  A
 * WITH_TREE_SITTER=1 build only happens on a box with a tree-sitter
 * prefix, and the same box's grammar prefixes are part of that contract
 * (doc/plans/kg-tree-sitter-plan.md, Refinement decision 1); a suite that
 * skipped itself here would make "the grammar is missing" indistinguishable
 * from "the backend paints nothing". */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/syntax.h"
#include "../src/syntax_tree_sitter_lang.h"
#include "test.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Helpers ---- */

static void setup(struct editor_syntax *syn)
{
	/* Before reset_current_buffer(), which memsets the record: the
	 * backend state is a pointer that lives in it, and a zeroed field is
	 * a leaked parser and tree (which is exactly what LeakSanitizer
	 * catches in the ASan lane). */
	syntax_state_release(bcur());
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	bcur()->syntax = syn;
}

static void teardown(void)
{
	syntax_state_release(bcur());
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

/* Put `lines` in the current buffer and give the backend the one
 * notification a whole-document change earns: syntax_rebuild(), which is
 * where a buffer with no state acquires one.  editor_insert_row() on its
 * own highlights each row against whatever tree exists, which for the
 * first row of a fresh buffer is none -- that is the compat path, not the
 * one an editor takes, and test_row_at_a_time_needs_a_tree() below pins
 * it deliberately. */
static void load_c(const char *const *lines, int n)
{
	int i;

	setup(syntax_find_by_name("C"));
	for (i = 0; i < n; i++) {
		editor_insert_row(bcur(), i, lines[i], strlen(lines[i]));
	}
	syntax_rebuild(bcur());
}

/* CHECK that row `r` of the current buffer has exactly `hl` -- one
 * expected face per byte of row->render, as a string of digits so the
 * expectation reads as a picture of the line above it. */
static void check_hl(int r, const char *expect)
{
	erow *row = &bcur()->row[r];
	int i, n = (int)strlen(expect);

	CHECKF(row->rsize == n, "row %d: rsize %d, expectation %d wide", r,
	    row->rsize, n);
	if (row->rsize != n) {
		return;
	}
	for (i = 0; i < n; i++) {
		CHECKF(row->hl[i] == (unsigned char)(expect[i] - '0'),
		    "row %d byte %d (%c): hl %d, expected %c", r, i,
		    row->chars ? row->render[i] : '?', row->hl[i], expect[i]);
	}
}

/* ---- Grammar loading and query compilation ---- */

/* The box contract: libtree-sitter-c.so is where the search path says. */
static void test_grammar_loads(void)
{
	char err[160];
	const TSLanguage *lang;

	err[0] = '\0';
	lang = kg_ts_grammar_load(
	    "c", kg_ts_grammar_search_path(), err, sizeof(err));
	CHECKF(lang != NULL, "C grammar did not load: %s", err);
}

/* A grammar that is not there is a miss with a reason, not a crash and not
 * a half-loaded language.  This is the whole of "unsupported languages
 * degrade to normal text": the caller gets NULL and paints nothing. */
static void test_missing_grammar_degrades(void)
{
	char err[160];

	err[0] = '\0';
	CHECK(
	    kg_ts_grammar_load("c", "/nonexistent/kg-ts-test", err, sizeof(err))
	    == NULL);
	CHECK(err[0] != '\0');

	err[0] = '\0';
	CHECK(kg_ts_grammar_load("no-such-language",
		  kg_ts_grammar_search_path(), err, sizeof(err))
	    == NULL);
	CHECK(err[0] != '\0');
}

/* Both spellings of a search-path entry resolve, and a path is searched in
 * order: a plain directory means <entry>/libtree-sitter-c.so, and an entry
 * containing %s has the grammar name substituted first, which is what lets
 * one entry cover a whole one-prefix-per-grammar tree. */
static void test_search_path_forms(void)
{
	char err[160];

	err[0] = '\0';
	CHECK(kg_ts_grammar_load(
		  "c", "/opt-9/tree-sitter-grammar-c/lib", err, sizeof(err))
	    != NULL);
	err[0] = '\0';
	CHECK(kg_ts_grammar_load(
		  "c", "/opt-9/tree-sitter-grammar-%s/lib", err, sizeof(err))
	    != NULL);
	/* Misses before the hit are skipped, not fatal, and empty entries
	 * are not "the current directory". */
	err[0] = '\0';
	CHECK(kg_ts_grammar_load("c",
		  "/nonexistent::/opt-9/tree-sitter-grammar-%s/lib", err,
		  sizeof(err))
	    != NULL);
}

/* $KG_TS_GRAMMAR_PATH wins over the compiled-in default when it is set to
 * something non-empty, and the default is what an unset or empty variable
 * means. */
static void test_env_overrides_search_path(void)
{
	char sopath[256];
	const char *dir = "test/.ts-grammar-env";
	char err[160];

	CHECK(strstr(kg_ts_grammar_search_path(), "%s") != NULL);
	setenv("KG_TS_GRAMMAR_PATH", "/somewhere/else", 1);
	CHECK(strcmp(kg_ts_grammar_search_path(), "/somewhere/else") == 0);
	setenv("KG_TS_GRAMMAR_PATH", "", 1);
	CHECK(strstr(kg_ts_grammar_search_path(), "%s") != NULL);
	unsetenv("KG_TS_GRAMMAR_PATH");

	/* And a directory that is only a grammar because a symlink says so
	 * loads exactly like the pinned prefix does. */
	if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
		CHECKF(0, "cannot make %s: %s", dir, strerror(errno));
		return;
	}
	snprintf(sopath, sizeof(sopath), "%s/libtree-sitter-c.so", dir);
	unlink(sopath);
	if (symlink(
		"/opt-9/tree-sitter-grammar-c/lib/libtree-sitter-c.so", sopath)
	    != 0) {
		CHECKF(0, "cannot symlink %s: %s", sopath, strerror(errno));
		return;
	}
	setenv("KG_TS_GRAMMAR_PATH", dir, 1);
	err[0] = '\0';
	CHECKF(kg_ts_grammar_load(
		   "c", kg_ts_grammar_search_path(), err, sizeof(err))
		!= NULL,
	    "%s", err);
	unsetenv("KG_TS_GRAMMAR_PATH");
	unlink(sopath);
	rmdir(dir);
}

/* kg's own highlight query compiles against the real grammar, and every
 * capture it names has a face.  A query that does not compile is reported
 * by kg_ts_language_for_mode() returning NULL, so this assertion is the
 * difference between "no colours because the query is broken" and "no
 * colours because the backend is broken". */
static void test_query_compiles(void)
{
	struct kg_ts_language *l = kg_ts_language_for_mode(KG_MODE_C);

	CHECK(l != NULL);
	if (!l) {
		return;
	}
	CHECK(l->state == KG_TS_LANG_READY);
	CHECK(l->query != NULL);
	CHECK(l->capture_count > 0 && l->capture_count <= KG_TS_MAX_CAPTURES);
	CHECK(kg_ts_language_for_mode(KG_MODE_C) == l); /* cached */
}

/* A mode with no registry row is not an error and costs nothing. */
static void test_unregistered_mode_has_no_language(void)
{
	CHECK(kg_ts_language_for_mode(KG_MODE_PYTHON) == NULL);
	CHECK(kg_ts_language_for_mode(KG_MODE_TEXT) == NULL);
}

/* ---- Captures to faces ---- */

/* The representative line: a type, an identifier, a number and a trailing
 * comment, asserted byte for byte. */
static void test_declaration_number_and_comment(void)
{
	static const char *const lines[] = { "int x = 42; /* c */" };

	load_c(lines, 1);
	/* int -> type, 42 -> number, the trailing comment to end of row. */
	check_hl(0, "5550000077002222222");
	teardown();
}

/* A string literal is one span whatever is inside it: the escape is part
 * of the literal, and the block-comment opener in the second string does
 * not open anything. */
static void test_string_literals(void)
{
	static const char *const lines[] = {
		"const char *s = \"a\\tb\";",
		"const char *t = \"/* not a comment\";",
	};

	load_c(lines, 2);
	check_hl(0, "44444055550000006666660");
	check_hl(1, "44444055550000006666666666666666660");
	teardown();
}

/* A block comment over three rows: the row that opens it from its opener,
 * the row in the middle whole, the row that closes it up to the closer --
 * and only up to it, so the code after the comment is coloured normally. */
static void test_multiline_block_comment(void)
{
	static const char *const lines[] = {
		"int a;",
		"/* opened",
		"still inside",
		"*/ int b;",
	};

	load_c(lines, 4);
	check_hl(0, "555000");
	check_hl(1, "222222222");
	check_hl(2, "222222222222");
	check_hl(3, "220555000");
	teardown();
}

/* Preprocessor lines: the directive is a keyword and the bracketed header
 * is a string, which is the one place C's lexer produces a string that no
 * quote character opens. */
static void test_preproc_directive(void)
{
	static const char *const lines[] = { "#include <stdio.h>" };

	load_c(lines, 1);
	check_hl(0, "444444440666666666");
	teardown();
}

/* THE coordinate seam.  A tab is one byte of row->chars and eight bytes of
 * row->render, and row->hl is indexed by the latter: a capture that
 * tree-sitter reports at chars column 1 has to be painted at render offset
 * 8 (doc/coordinates.md rows 3 and 4).  Getting this wrong is invisible in
 * a saved file and obvious on a screen. */
static void test_tab_before_token(void)
{
	static const char *const lines[] = { "\tint x;" };
	erow *row;

	load_c(lines, 1);
	row = &bcur()->row[0];
	CHECK(row->size == 7);
	CHECK(row->rsize == 14); /* one tab -> eight columns */
	CHECK(row->hl[7] == HL_NORMAL); /* last column of the tab */
	CHECK(row->hl[8] == HL_KEYWORD2);
	CHECK(row->hl[9] == HL_KEYWORD2);
	CHECK(row->hl[10] == HL_KEYWORD2);
	CHECK(row->hl[11] == HL_NORMAL);
	teardown();
}

/* And the other half of it: a multi-byte character is BYTES, not one
 * column, on both sides of the conversion.  "e-acute" is two bytes of
 * chars and two bytes of render, so the token after it must not slide. */
static void test_utf8_before_token(void)
{
	/* 21 bytes; the "int" starts at byte 16 on both sides. */
	static const char *const lines[] = { "char *s = \"\xc3\xa9\"; int x;" };
	erow *row;

	load_c(lines, 1);
	row = &bcur()->row[0];
	CHECK(row->size == 22);
	CHECK(row->rsize == 22);
	check_hl(0, "5555000000666600555000");
	teardown();
}

/* ---- Lifecycle ---- */

/* A staged load -- rows that belong to no buffer yet -- comes out parsed
 * and painted, with a state for the buffer that adopts it.  This is the
 * only highlighting pass a file load pays for. */
static void test_prepare_rows_parses_and_paints(void)
{
	struct editor_syntax *c = syntax_find_by_name("C");
	struct kg_syntax_state *st;
	erow *rows = NULL;
	int numrows = 0, cap = 0, ok = 0;

	setup(c);
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "/* open", 7));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "still", 5));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "*/ int x;", 9));
	CHECK(kg_row_builder_render(rows, numrows) == 0);

	st = syntax_prepare_rows(rows, numrows, c, &ok);
	CHECK(ok == 1);
	CHECK(st != NULL);
	CHECK(rows[0].hl != NULL && rows[0].hl[0] == HL_COMMENT);
	CHECK(rows[1].hl != NULL && rows[1].hl[0] == HL_COMMENT);
	CHECK(rows[2].hl != NULL && rows[2].hl[0] == HL_COMMENT);
	CHECK(rows[2].hl[3] == HL_KEYWORD2); /* the int after the closer */
	syntax_state_discard(st);
	kg_row_builder_free(&rows, &numrows, &cap);
	teardown();
}

/* An edit through the gateway reaches the backend once, and slice 6
 * answers it with a full reparse: the rows the edit produced, and the rows
 * below them, are all correct afterwards.  The edit here opens a block
 * comment where there was none, which is the case a backend that only
 * looked at the replaced rows would get wrong. */
static void test_edit_reparses_whole_buffer(void)
{
	static const char *const lines[] = { "int a;", "int b;", "int c;" };
	struct kg_edit e;
	size_t begin, end;

	load_c(lines, 3);
	check_hl(0, "555000");
	check_hl(1, "555000");

	/* One transaction that replaces row 0 with two rows of block
	 * comment.  A backend that only re-examined the replaced span would
	 * still be right about rows 2 and 3 here; what this pins is that the
	 * multi-row replacement itself comes out right, offsets and all,
	 * from a tree that was thrown away and built again. */
	begin = buffer_row_col_to_position(bcur(), 0, 0);
	end = buffer_row_col_to_position(bcur(), 0, 6);
	e = kg_edit_user(bcur(), begin, end, "/* was a\nstill comment */", 25);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	CHECK(bcur()->numrows == 4);
	check_hl(0, "22222222");
	check_hl(1, "2222222222222222");
	check_hl(2, "555000");
	check_hl(3, "555000");
	teardown();
}

/* A buffer gets its state from the notification, and loses it the moment
 * its mode stops being one this backend parses.  b->syntax_state is the
 * pointer def.h keeps; what is behind it is nobody's business here. */
static void test_mode_change_acquires_and_releases_state(void)
{
	static const char *const lines[] = { "int x;" };

	load_c(lines, 1);
	CHECK(bcur()->syntax_state != NULL);
	check_hl(0, "555000");

	editor_set_syntax(bcur(), syntax_find_by_name("Python"));
	CHECK(bcur()->syntax_state == NULL);
	check_hl(0, "000000");

	editor_set_syntax(bcur(), syntax_find_by_name("C"));
	CHECK(bcur()->syntax_state != NULL);
	check_hl(0, "555000");
	teardown();
}

/* A mode with no grammar is plain text, with no state and no crash --
 * including through the row-at-a-time path, which is what an editor
 * command that touches one row uses. */
static void test_unsupported_mode_is_plain_text(void)
{
	static const char *const lines[] = { "def f(): return 42" };
	int i;

	setup(syntax_find_by_name("Python"));
	editor_insert_row(bcur(), 0, lines[0], strlen(lines[0]));
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state == NULL);
	for (i = 0; i < bcur()->row[0].rsize; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NORMAL);
	}
	editor_update_syntax(bcur(), &bcur()->row[0]);
	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	teardown();
}

/* The compat path, pinned rather than assumed: a row re-highlighted on its
 * own is answered from whatever tree the buffer currently has -- correctly
 * once there is one, and HL_NORMAL before there is.  Every editor path
 * that changes text goes through the gateway and therefore through
 * syntax_backend_after_edit(); this is what the leftovers get. */
static void test_row_at_a_time_needs_a_tree(void)
{
	static const char *const lines[] = { "int x;" };

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, lines[0], strlen(lines[0]));
	check_hl(0, "000000"); /* no tree yet */
	syntax_rebuild(bcur());
	check_hl(0, "555000");
	editor_update_syntax(bcur(), &bcur()->row[0]);
	check_hl(0, "555000"); /* re-queried from the tree */
	teardown();
}

/* An empty row has no hl at all (the facade frees it), and a buffer of
 * nothing but empty rows must still parse and paint without reaching for
 * one. */
static void test_empty_rows(void)
{
	static const char *const lines[] = { "", "int x;", "" };

	load_c(lines, 3);
	CHECK(bcur()->row[0].hl == NULL);
	CHECK(bcur()->row[2].hl == NULL);
	check_hl(1, "555000");
	teardown();
}

/* ---- A real file ---- */

/* The end-to-end claim, and the only one that says the backend produces
 * something a person would see: a real source file comes out with a lot of
 * non-HL_NORMAL bytes, including types and comments.  Run from the repo
 * root, which is where `make check` runs it. */
static void test_real_source_file_is_colourful(void)
{
	static const char *const candidates[]
	    = { "src/main.c", "../src/main.c" };
	char line[4096];
	int painted = 0, keyword2 = 0, comment = 0, i, j, n = 0;
	FILE *fp = NULL;

	setup(syntax_find_by_name("C"));
	for (i = 0; i < 2 && !fp; i++) {
		fp = fopen(candidates[i], "r");
	}
	CHECK(fp != NULL);
	if (!fp) {
		teardown();
		return;
	}
	while (fgets(line, sizeof(line), fp)) {
		size_t len = strlen(line);

		while (len > 0
		    && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			len--;
		}
		editor_insert_row(bcur(), n++, line, len);
	}
	fclose(fp);
	syntax_rebuild(bcur());
	CHECK(bcur()->syntax_state != NULL);
	for (i = 0; i < bcur()->numrows; i++) {
		erow *row = &bcur()->row[i];

		for (j = 0; j < row->rsize; j++) {
			painted += row->hl[j] != HL_NORMAL;
			keyword2 += row->hl[j] == HL_KEYWORD2;
			comment += row->hl[j] == HL_COMMENT;
		}
	}
	CHECKF(painted > 100, "only %d painted bytes over %d rows", painted,
	    bcur()->numrows);
	CHECK(keyword2 > 0);
	CHECK(comment > 0);
	teardown();
}

/* ---- The differential: the incremental path against the full one -------
 *
 * The regression test doc/plans/kg-tree-sitter-plan.md calls the most
 * valuable one, and the thing that gives Phase 7 permission to repaint
 * fewer rows than it changed: apply an edit through the gateway, so the
 * backend answers it incrementally (ts_tree_edit() plus a parse against
 * the old tree, plus a repaint of the damaged rows only), then throw the
 * whole parse away and do the same text from nothing.  Every byte of every
 * row's hl must be the same.  Anything the damage window misses -- a row
 * whose colour changed without tree-sitter calling it changed, an offset
 * that survived the edit in the wrong coordinate space -- shows up here as
 * a difference, and nowhere else.
 *
 * Two layers, as the plan asks: a table of awkward edits, each named, and
 * a seeded random loop that prints its seed so a failure reproduces. */

/* The fixture every case starts from.  Every construct in it is one whose
 * colouring depends on text that is not on its own row: a comment spanning
 * rows, a string containing what would otherwise open one, preprocessor
 * lines, nested braces, a tab before a token and a multi-byte character
 * before one. */
static const char *const diff_fixture[] = {
	"#include <stdio.h>",
	"#define MAX(a, b) ((a) > (b) ? (a) : (b))",
	"",
	"/* A block comment",
	" * spanning three rows.",
	" */",
	"static const char *msg = \"hello /* not a comment */ world\";",
	"static const char *utf = \"caf\xc3\xa9\t\\ttab\";",
	"",
	"int main(int argc, char **argv)",
	"{",
	"\tint i = 0; /* trailing */",
	"\tfor (i = 0; i < 10; i++) {",
	"\t\tprintf(\"%d\\n\", i);",
	"\t}",
	"\tif (argc > 1) {",
	"\t\treturn 1; /* one */",
	"\t}",
	"\treturn 0;",
	"}",
};

#define DIFF_FIXTURE_ROWS ((int)(sizeof(diff_fixture) / sizeof(*diff_fixture)))

/* A column that means "the end of whatever row this is":
 * buffer_row_col_to_position() clamps a column to the row's size, so a
 * case can name a row's end without knowing its width. */
#define EOL 100000

/* Every row's hl in two flat arenas -- the widths, and the bytes
 * concatenated -- so that comparing two of them is a memcmp and not a walk
 * of a structure.  An empty row contributes no bytes and a zero width,
 * which is exactly what the facade leaves behind for one. */
struct hl_snap {
	int numrows;
	int *rsize;
	size_t total;
	unsigned char *hl;
};

static void snap_free(struct hl_snap *s)
{
	free(s->rsize);
	free(s->hl);
	*s = (struct hl_snap) { 0 };
}

static int snap_take(struct editor_buffer *b, struct hl_snap *s)
{
	size_t at = 0;
	int i;

	*s = (struct hl_snap) { 0 };
	s->numrows = b->numrows;
	for (i = 0; i < b->numrows; i++) {
		s->total += (size_t)b->row[i].rsize;
	}
	s->rsize = calloc(
	    (size_t)(b->numrows > 0 ? b->numrows : 1), sizeof(*s->rsize));
	s->hl = calloc(s->total + 1, 1);
	if (!s->rsize || !s->hl) {
		snap_free(s);
		return 0;
	}
	for (i = 0; i < b->numrows; i++) {
		s->rsize[i] = b->row[i].rsize;
		if (b->row[i].rsize > 0) {
			memcpy(
			    s->hl + at, b->row[i].hl, (size_t)b->row[i].rsize);
			at += (size_t)b->row[i].rsize;
		}
	}
	return 1;
}

/* The whole arena in one memcmp: what the differential actually asks. */
static int snap_identical(const struct hl_snap *a, const struct hl_snap *b)
{
	return a->numrows == b->numrows && a->total == b->total
	    && memcmp(a->hl, b->hl, a->total) == 0
	    && memcmp(a->rsize, b->rsize,
		   sizeof(*a->rsize)
		       * (size_t)(a->numrows > 0 ? a->numrows : 1))
	    == 0;
}

/* Byte-identical, or the first byte that is not, with the row and offset
 * it is at and which side said what: the memcmp above answers "different"
 * and this answers "where", which is the difference between a failure that
 * is a bug report and one that is a puzzle. */
static void snap_expect_equal(const struct hl_snap *a, const struct hl_snap *b,
    const char *what, const char *lhs, const char *rhs)
{
	size_t at = 0;
	int i, j;

	if (snap_identical(a, b)) {
		return;
	}
	CHECKF(a->numrows == b->numrows, "%s: %d rows %s, %d %s", what,
	    a->numrows, lhs, b->numrows, rhs);
	if (a->numrows != b->numrows) {
		return;
	}
	for (i = 0; i < a->numrows; i++) {
		CHECKF(a->rsize[i] == b->rsize[i],
		    "%s: row %d is %d wide %s, %d %s", what, i, a->rsize[i],
		    lhs, b->rsize[i], rhs);
		if (a->rsize[i] != b->rsize[i]) {
			return;
		}
		for (j = 0; j < a->rsize[i]; j++) {
			CHECKF(a->hl[at + j] == b->hl[at + j],
			    "%s: row %d byte %d: hl %d %s, %d %s", what, i, j,
			    a->hl[at + j], lhs, b->hl[at + j], rhs);
			if (a->hl[at + j] != b->hl[at + j]) {
				return;
			}
		}
		at += (size_t)a->rsize[i];
	}
}

/* The comparison itself, and it is deliberately THREE snapshots, because
 * "the incremental answer differs from the full one" has two possible
 * causes and they belong to different projects:
 *
 *   inc   what the incremental edit left behind -- a parse against the
 *         edited old tree, and a repaint of the damaged rows only;
 *   wide  every row of that same tree repainted (editor_rehighlight_all()
 *         re-colours without reparsing), so it is what the incremental
 *         TREE says, with no damage window in the way;
 *   full  the state released -- parser, tree and all -- and the same text
 *         parsed by a parser that has never seen this document.
 *
 * inc == wide is KG's property and is checked strictly, always: it says
 * the damage window covered every row whose colour this tree implies, and
 * it is exactly what Phase 7 narrowed and therefore exactly what can be
 * wrong.  A row the window missed shows up here and nowhere else.
 *
 * wide == full is TREE-SITTER's property, and it is not one tree-sitter
 * promises for text containing syntax errors: an incremental parse of
 * broken input may recover differently from a fresh parse of the same
 * bytes, and both are legitimate trees.  Every deliberate case in the
 * table below holds it -- which is why they assert it -- and the random
 * loop counts the divergences it finds instead of failing on each one,
 * because a random edit produces broken C almost by definition.
 * Measured while writing this: 7 divergences in 18 900 random edits over
 * 63 seeds, none of them at the default seed, and every one of the five
 * examined with a has-error probe was on text that BOTH parses called
 * erroneous.
 *
 * The buffer is left holding the fresh state, so the next edit in a
 * sequence is an incremental one against a tree that is known good. */
static int differential_check_ex(const char *what, int strict_tree)
{
	struct hl_snap inc = { 0 }, wide = { 0 }, full = { 0 };
	int diverged;

	if (!snap_take(bcur(), &inc)) {
		CHECKF(0, "%s: out of memory taking the snapshot", what);
		return 0;
	}
	editor_rehighlight_all(bcur());
	if (!snap_take(bcur(), &wide)) {
		CHECKF(0, "%s: out of memory taking the snapshot", what);
		snap_free(&inc);
		return 0;
	}
	syntax_state_release(bcur());
	syntax_rebuild(bcur());
	if (!snap_take(bcur(), &full)) {
		CHECKF(0, "%s: out of memory taking the snapshot", what);
		snap_free(&inc);
		snap_free(&wide);
		return 0;
	}
	snap_expect_equal(&inc, &wide, what, "in the damaged rows",
	    "with every row repainted");
	diverged = !snap_identical(&wide, &full);
	if (strict_tree) {
		snap_expect_equal(&wide, &full, what,
		    "from the incremental tree", "from a fresh parse");
	}
	snap_free(&inc);
	snap_free(&wide);
	snap_free(&full);
	return diverged;
}

/* The whole differential, both halves strict: what a named, deliberate
 * case gets. */
static void differential_check(const char *what)
{
	differential_check_ex(what, 1);
}

/* One edit, in the terms the case table is written in.  Positions are
 * resolved against the buffer as it is now, so a table's rows can be
 * applied one after another as well as one at a time. */
static int apply_edit(int r0, int c0, int r1, int c1, const char *text)
{
	size_t begin = buffer_row_col_to_position(bcur(), r0, c0);
	size_t end = buffer_row_col_to_position(bcur(), r1, c1);
	struct kg_edit e = kg_edit_user(bcur(), begin, end, text, strlen(text));

	return kg_buffer_replace(&e, NULL);
}

struct diff_case {
	const char *name;
	int r0, c0, r1, c1;
	const char *text;
};

/* The awkward edits: each one changes what rows far away from it mean, or
 * sits on a boundary where an off-by-one would live. */
static const struct diff_case diff_cases[] = {
	{ "open a block comment at the top", 0, 0, 0, 0, "/*" },
	{ "close a comment that was never opened", 0, 0, 0, 0, "*/" },
	{ "delete a block comment's opener", 3, 0, 3, 2, "" },
	{ "delete a block comment's closer", 5, 1, 5, 3, "" },
	{ "split a string across two rows", 6, 30, 6, 30, "\n" },
	{ "join the two rows a comment spans", 3, EOL, 4, 0, "" },
	{ "paste over a comment boundary", 4, 0, 6, 6, "*/ int x;\n/* re" },
	{ "delete the comment whole", 3, 0, 6, 0, "" },
	{ "edit at byte zero", 0, 0, 0, 1, "/" },
	{ "edit at the last byte", 19, 0, 19, EOL, "/*" },
	{ "append past the last byte", 19, EOL, 19, EOL, "\nint tail;" },
	{ "open a string at the top", 0, 0, 0, 0, "\"" },
	{ "a tab before a token", 11, 0, 11, 0, "\t" },
	{ "a multi-byte character before a token", 11, 1, 11, 1, "\xc3\xa9" },
	{ "delete everything", 0, 0, 19, EOL, "" },
	{ "replace everything with a comment", 0, 0, 19, EOL, "/* all */" },
};

/* Each awkward edit on its own, against a fresh fixture: an incremental
 * answer and a from-scratch one, byte for byte. */
static void test_differential_case_table(void)
{
	size_t i;

	for (i = 0; i < sizeof(diff_cases) / sizeof(*diff_cases); i++) {
		const struct diff_case *c = &diff_cases[i];

		load_c(diff_fixture, DIFF_FIXTURE_ROWS);
		CHECKF(apply_edit(c->r0, c->c0, c->r1, c->c1, c->text) == 1,
		    "%s: the edit was refused", c->name);
		differential_check(c->name);
		teardown();
	}
}

/* The same edits again, this time one after another over the same buffer
 * with no reset between them, because an incremental parse is normally
 * given a tree that another incremental parse produced.  The check after
 * each one keeps a failure attributable to the edit that caused it. */
static void test_differential_chained_edits(void)
{
	static const struct diff_case chain[] = {
		{ "chain: open a comment", 0, 0, 0, 0, "/*" },
		{ "chain: close it two rows down", 2, 0, 2, 0, "*/" },
		{ "chain: delete the closer again", 2, 0, 2, 2, "" },
		{ "chain: open a string on row 1", 1, 0, 1, 0, "\"" },
		{ "chain: type into the string", 1, 1, 1, 1, "text /* x" },
		{ "chain: close the comment at the end", 3, EOL, 3, EOL,
		    " */" },
		{ "chain: join two rows", 0, EOL, 1, 0, "" },
	};
	size_t i;

	load_c(diff_fixture, DIFF_FIXTURE_ROWS);
	for (i = 0; i < sizeof(chain) / sizeof(*chain); i++) {
		CHECKF(apply_edit(chain[i].r0, chain[i].c0, chain[i].r1,
			   chain[i].c1, chain[i].text)
			== 1,
		    "%s: the edit was refused", chain[i].name);
		differential_check(chain[i].name);
	}
	teardown();
}

/* Typing into a buffer that has nothing in it -- the one starting state
 * the fixture cannot produce, and the one where the edit's start point is
 * the only position that exists. */
static void test_differential_empty_buffer(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "", 0);
	syntax_rebuild(bcur());
	CHECK(apply_edit(0, 0, 0, 0, "i") == 1);
	differential_check("empty buffer: first character");
	CHECK(apply_edit(0, 1, 0, 1, "nt x;\n/* c") == 1);
	differential_check("empty buffer: a comment opener");
	CHECK(apply_edit(0, 0, 1, EOL, "") == 1);
	differential_check("empty buffer: emptied again");
	teardown();
}

/* ---- The seeded random loop ---- */

/* xorshift32, so the sequence is the same on every box a seed is quoted
 * on; rand() promises nothing of the sort. */
static unsigned int diff_rand(unsigned int *state)
{
	unsigned int x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* The alphabet a random replacement is built from: the tokens that change
 * what the text around them means, weighted by being listed at all. */
static const char *const diff_tokens[] = {
	"/*",
	"*/",
	"\"",
	"'",
	"\\",
	"\n",
	"\n\n",
	";",
	"{",
	"}",
	"(",
	")",
	"int ",
	"x",
	"//",
	"#define ",
	"\t",
	" ",
	"\xc3\xa9",
	"return 0;",
	"#if",
	"#endif",
	"0x2a",
	"'c'",
};

#define DIFF_TOKENS ((unsigned int)(sizeof(diff_tokens) / sizeof(*diff_tokens)))

/* Up to three tokens, concatenated.  Returns the length. */
static size_t diff_random_text(unsigned int *state, char *out, size_t outsz)
{
	unsigned int n = diff_rand(state) % 4;
	size_t len = 0;
	unsigned int i;

	out[0] = '\0';
	for (i = 0; i < n; i++) {
		const char *tok = diff_tokens[diff_rand(state) % DIFF_TOKENS];
		size_t tl = strlen(tok);

		if (len + tl + 1 >= outsz) {
			break;
		}
		memcpy(out + len, tok, tl);
		len += tl;
	}
	out[len] = '\0';
	return len;
}

/* The span an edit replaces: usually a few bytes, occasionally a big one,
 * because a buffer that only ever grows and shrinks by three bytes never
 * makes the parser re-lay a whole subtree. */
static void diff_random_span(
    unsigned int *state, size_t total, size_t *begin, size_t *end)
{
	size_t at = (size_t)diff_rand(state) % (total + 1);
	size_t width = (diff_rand(state) % 8 == 0)
	    ? (size_t)diff_rand(state) % 64
	    : (size_t)diff_rand(state) % 6;

	*begin = at;
	*end = at + width > total ? total : at + width;
}

/* Random edits, checked after every one.  The seed is printed with every
 * failure and fixed by default, so CI runs the same 200 edits each time
 * and a hunt can run different ones: KG_TS_DIFF_SEED and KG_TS_DIFF_CASES
 * are the knobs, the same shape make check-regex-differential uses.
 *
 * The damage window is checked strictly on every edit; the tree
 * comparison is counted rather than asserted, for the reason
 * differential_check_ex() gives at length -- tree-sitter does not promise
 * that an incremental parse of broken input recovers the way a fresh parse
 * of the same bytes does, and random edits produce broken input on
 * purpose.  The count is printed when it is not zero so that a change
 * which makes it jump is visible rather than silent. */
static void test_differential_random_edits(void)
{
	unsigned int seed0 = 0x9e3779b9u;
	unsigned int state;
	int cases = 200;
	int applied = 0, diverged = 0;
	const char *env;
	char text[128];
	char what[256];
	int i;

	env = getenv("KG_TS_DIFF_SEED");
	if (env && *env) {
		seed0 = (unsigned int)strtoul(env, NULL, 0);
	}
	env = getenv("KG_TS_DIFF_CASES");
	if (env && *env) {
		cases = (int)strtol(env, NULL, 0);
	}
	state = seed0 ? seed0 : 1u;
	load_c(diff_fixture, DIFF_FIXTURE_ROWS);
	for (i = 0; i < cases; i++) {
		size_t begin, end, len;
		struct kg_edit e;

		diff_random_span(
		    &state, buffer_byte_length(bcur()), &begin, &end);
		len = diff_random_text(&state, text, sizeof(text));
		e = kg_edit_user(bcur(), begin, end, text, len);
		if (!kg_buffer_replace(&e, NULL)) {
			continue;
		}
		snprintf(what, sizeof(what),
		    "seed 0x%x case %d: [%zu,%zu) <- %zu bytes", seed0, i,
		    begin, end, len);
		applied++;
		diverged += differential_check_ex(what, 0);
	}
	if (diverged) {
		fprintf(stderr,
		    "  (seed 0x%x: %d of %d edits recovered from a syntax "
		    "error differently than a fresh parse)\n",
		    seed0, diverged, applied);
	}
	/* The shape is still asserted even though the individual case is
	 * not.  Error recovery diverges on a fraction of a percent of edits
	 * (7 in 18 900 measured); a wrong TSInputEdit, or an old tree that
	 * was never told about the edit, diverges on nearly all of them, and
	 * that is what this catches. */
	CHECKF(diverged * 10 <= applied,
	    "seed 0x%x: %d of %d edits disagreed with a fresh parse, which is "
	    "too many to be error recovery",
	    seed0, diverged, applied);
	teardown();
}

int main(void)
{
	RUN(test_grammar_loads);
	RUN(test_missing_grammar_degrades);
	RUN(test_search_path_forms);
	RUN(test_env_overrides_search_path);
	RUN(test_query_compiles);
	RUN(test_unregistered_mode_has_no_language);
	RUN(test_declaration_number_and_comment);
	RUN(test_string_literals);
	RUN(test_multiline_block_comment);
	RUN(test_preproc_directive);
	RUN(test_tab_before_token);
	RUN(test_utf8_before_token);
	RUN(test_prepare_rows_parses_and_paints);
	RUN(test_edit_reparses_whole_buffer);
	RUN(test_mode_change_acquires_and_releases_state);
	RUN(test_unsupported_mode_is_plain_text);
	RUN(test_row_at_a_time_needs_a_tree);
	RUN(test_empty_rows);
	RUN(test_real_source_file_is_colourful);
	RUN(test_differential_case_table);
	RUN(test_differential_chained_edits);
	RUN(test_differential_empty_buffer);
	RUN(test_differential_random_edits);
	return test_summary();
}
