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
	return test_summary();
}
