/* test_syntax.c -- regression tests for the backend-neutral syntax layer
 *
 * What a mode is, which mode a file name or shebang selects, what an HL_*
 * face is in terminal colours, the mode-change event, the mode-owned
 * highlighter hook, and the lifetime of the backend's opaque per-buffer
 * state.  Every "this byte comes out HL_KEYWORD1" assertion belongs to
 * whichever backend is linked and lives in that backend's own suite --
 * test/test_syntax_legacy.c for the bespoke scanners.
 *
 * This suite is built and run in EVERY configuration, so nothing in it may
 * assume a particular backend.  Where a colour is asserted here it is
 * painted by a mode-owned highlighter the test itself supplies (dired's
 * arrangement), which no backend is allowed to override; where the answer
 * is genuinely a scanner's, the assertion lives next door and a comment
 * here says so. */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/syntax.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Helpers ---- */

static void setup(struct editor_syntax *syn)
{
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
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

/* ---- is_separator tests ---- */

static void dummy_highlight(struct editor_buffer *b, struct erow *row)
{
	(void)b;

	int i;
	for (i = 0; i < row->rsize; i++) {
		row->hl[i] = HL_KEYWORD1;
	}
}

static int custom_hl_count = 0;
static void counting_highlight(struct editor_buffer *b, struct erow *row)
{
	(void)b;

	(void)row;
	custom_hl_count++;
}

/* Character classification is ASCII-only where the grammar is.  These
 * assertions are the same under -fsigned-char and -funsigned-char, which
 * .ci/ci-11-char-signedness.sh checks by running this binary both ways;
 * <ctype.h> on an uncast char is undefined for a negative value, and in
 * the "C" locale says nothing useful about a byte >= 0x80 either. */
static void test_ascii_classification_is_sign_independent(void)
{
	CHECK(ascii_is_print(' ') && ascii_is_print('~'));
	CHECK(!ascii_is_print(0x1f) && !ascii_is_print(0x7f));
	CHECK(!ascii_is_print(0xc3) && !ascii_is_print(-61));
	/* Soft key codes live above 0xFF and are not text. */
	CHECK(!ascii_is_print(ARROW_LEFT) && !ascii_is_print(DEL_KEY));
	CHECK(ascii_is_digit('0') && ascii_is_digit('9'));
	CHECK(!ascii_is_digit('a') && !ascii_is_digit(0xb9) /* superscript */);
	CHECK(ascii_is_space(' ') && ascii_is_space('\t')
	    && ascii_is_space('\n') && ascii_is_space('\r'));
	CHECK(!ascii_is_space('x') && !ascii_is_space(0xa0) /* NBSP byte */);
	CHECK(!ascii_is_space(-96));
	/* is_separator() is the legacy backend's, so the same assertion
	 * against a negative char lives in test_syntax_legacy.c, which is
	 * the suite that links it. */
}

static void test_syntax_to_color(void)
{
	CHECK(editor_syntax_to_color(HL_COMMENT) == 36);
	CHECK(editor_syntax_to_color(HL_MLCOMMENT) == 36);
	CHECK(editor_syntax_to_color(HL_KEYWORD1) == 33);
	CHECK(editor_syntax_to_color(HL_KEYWORD2) == 32);
	CHECK(editor_syntax_to_color(HL_STRING) == 35);
	CHECK(editor_syntax_to_color(HL_NUMBER) == 31);
	CHECK(editor_syntax_to_color(HL_MATCH) == 34);
	CHECK(editor_syntax_to_color(HL_NORMAL) == 37);
}

/* ---- C syntax tests (KG_MODE_C) ---- */

/* Comment on row 0, text on row 1: row 1 is the subject, not row 0.
 * Also a single sequential insert with no re-trigger. */
static void test_gitcommit_subject_skips_comment(void)
{
	setup(syntax_find_by_name("Git commit"));
	editor_insert_row(bcur(), 0, "# leading comment", 17);
	editor_insert_row(bcur(), 1, "the subject", 11);

	CHECK(syntax_git_commit_subject() == 1);
	CHECK(bcur()->row[1].hl[0] == HL_NORMAL);
	teardown();
}

/* A buffer of only comments has no subject.  What a comment row is
 * *painted* is the backend's answer, and test_syntax_legacy.c's
 * test_gitcommit_comment_line() is where it is asserted; this is the
 * facade's question, which every backend answers the same. */
static void test_gitcommit_no_subject(void)
{
	setup(syntax_find_by_name("Git commit"));
	editor_insert_row(bcur(), 0, "# only a comment", 16);
	editor_insert_row(bcur(), 1, "# another comment", 17);

	CHECK(syntax_git_commit_subject() == -1);
	teardown();
}

/* ---- Git rebase todo syntax tests ---- */

/* syntax_git_rebase_pick_span: accepts commit-taking actions (long or
 * abbreviated, indented), refuses everything else. */
static void test_gitrebase_pick_span(void)
{
	int start, wlen;

	CHECK(syntax_git_rebase_pick_span("pick 1a2b3c4 x", 14, &start, &wlen)
	    && start == 0 && wlen == 4);
	CHECK(syntax_git_rebase_pick_span("  r 1a2b3c4 x", 13, &start, &wlen)
	    && start == 2 && wlen == 1);
	CHECK(syntax_git_rebase_pick_span("drop 1a2b3c4", 12, &start, &wlen)
	    && start == 0 && wlen == 4);
	CHECK(!syntax_git_rebase_pick_span("exec make", 9, &start, &wlen));
	CHECK(!syntax_git_rebase_pick_span("label onto", 10, &start, &wlen));
	CHECK(!syntax_git_rebase_pick_span("# pick x", 8, &start, &wlen));
	CHECK(!syntax_git_rebase_pick_span("", 0, &start, &wlen));
	CHECK(!syntax_git_rebase_pick_span("picked 1a2b", 11, &start, &wlen));
}

/* Mode selection: exact basename only.  A path merely containing the
 * string must not select the rebase mode (its keys quit the editor). */
static void test_gitrebase_detect_basename_only(void)
{
	setup(NULL);
	editor_select_syntax_highlight(
	    bcur(), ".git/rebase-merge/git-rebase-todo");
	CHECK(bcur()->syntax == syntax_find_by_name("Git rebase"));

	bcur()->syntax = NULL;
	editor_select_syntax_highlight(bcur(), "git-rebase-todo");
	CHECK(bcur()->syntax == syntax_find_by_name("Git rebase"));

	bcur()->syntax = NULL;
	editor_select_syntax_highlight(
	    bcur(), "notes-on-git-rebase-todo-things");
	CHECK(bcur()->syntax != syntax_find_by_name("Git rebase"));
	teardown();
}

/* syntax_git_rebase_flags_end: end of the option words after `from`,
 * or `from` itself when none follow. */
static void test_gitrebase_flags_end(void)
{
	CHECK(syntax_git_rebase_flags_end("fixup -C 1a2b", 13, 5) == 8);
	CHECK(syntax_git_rebase_flags_end("pick 1a2b", 9, 4) == 4);
	CHECK(syntax_git_rebase_flags_end("f -C -c x", 9, 1) == 7);
	CHECK(syntax_git_rebase_flags_end("fixup -C", 8, 5) == 8);
	CHECK(syntax_git_rebase_flags_end("fixup   ", 8, 5) == 5);
}

/* Commit-mode selection: exact basename only, like the rebase todo (the
 * mode's keys quit the editor). */
static void test_gitcommit_detect_basename_only(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), ".git/COMMIT_EDITMSG");
	CHECK(bcur()->syntax == syntax_find_by_name("Git commit"));

	bcur()->syntax = NULL;
	editor_select_syntax_highlight(bcur(), "MERGE_MSG");
	CHECK(bcur()->syntax == syntax_find_by_name("Git commit"));

	bcur()->syntax = NULL;
	editor_select_syntax_highlight(bcur(), "my-COMMIT_EDITMSG-notes");
	CHECK(bcur()->syntax != syntax_find_by_name("Git commit"));
	teardown();
}

/* Sub-plan 02D's dialect cutover: kg's Lisp is .el, and .fe (Fe's own
 * standalone dialect) is no longer a Lisp-mode extension in kg -- a pure
 * registry lookup, so no tmux screen assertion is needed here. */
static void test_lisp_el_extension_selects(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "config.el");
	CHECK(bcur()->syntax == syntax_find_by_name("Lisp"));
	teardown();
}

static void test_lisp_fe_extension_unchanged(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "config.fe");
	CHECK(bcur()->syntax == NULL);
	teardown();
}

/* Custom highlighter hook test */
static void test_custom_highlighter_pointer(void)
{
	struct editor_syntax dummy_syntax
	    = { KG_MODE_TEXT, "Dummy", NULL, "", dummy_highlight };
	setup(&dummy_syntax);
	editor_insert_row(bcur(), 0, "hello", 5);
	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	teardown();
}

/* editor_set_syntax() rehighlights the rows the buffer already holds.  Both
 * modes here own their highlighter outright, so what this pins is the
 * facade's rebuild rather than any backend's opinion of the text: the row
 * is painted under the first mode, the second mode's hook is called once
 * for it, and what that hook does not paint comes back HL_NORMAL.  The same
 * transition between two *scanned* registry modes is
 * test_syntax_legacy.c's. */
static void test_editor_set_syntax_rebuilds(void)
{
	struct editor_syntax painting
	    = { KG_MODE_TEXT, "Painting", NULL, "", dummy_highlight };
	struct editor_syntax counting
	    = { KG_MODE_TEXT, "Counting", NULL, "", counting_highlight };

	setup(&painting);
	editor_insert_row(bcur(), 0, "int x;", 6);
	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);

	custom_hl_count = 0;
	editor_set_syntax(bcur(), &counting);
	CHECK(custom_hl_count == 1);
	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	teardown();
}

static void test_rehighlight_all_linear_complexity(void)
{
	int i;
	struct editor_syntax counting_syntax
	    = { KG_MODE_TEXT, "Counting", NULL, "", counting_highlight };
	setup(&counting_syntax);

	for (i = 0; i < 200; i++) {
		editor_insert_row(bcur(), i, "text", 4);
	}

	custom_hl_count = 0;
	editor_rehighlight_all(bcur());

	CHECK(custom_hl_count == 200);
	teardown();
}

static void test_editor_set_syntax_persistence(void)
{
	setup(syntax_find_by_name("C"));
	editor_set_syntax(bcur(), syntax_find_by_name("C"));
	CHECK(bcur()->syntax == syntax_find_by_name("C"));
	CHECK(buflist[buf_current].syntax == syntax_find_by_name("C"));

	editor_set_syntax(bcur(), syntax_find_by_name("Markdown"));
	CHECK(bcur()->syntax == syntax_find_by_name("Markdown"));
	CHECK(buflist[buf_current].syntax == syntax_find_by_name("Markdown"));
	teardown();
}

/* ".go" is the whole of Go mode's file-name rule: a go.mod is the module's
 * manifest, and is a workspace marker (src/lsp_server.c) rather than Go
 * source.  The selection matters beyond colours -- the mode is what picks
 * the language server. */
static void test_go_selection_go_extension(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "main.go");
	CHECK(bcur()->syntax == syntax_find_by_name("Go"));
	CHECK(bcur()->syntax && bcur()->syntax->id == KG_MODE_GO);
	teardown();
}

static void test_go_selection_go_mod_unchanged(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "go.mod");
	CHECK(bcur()->syntax == NULL);
	teardown();
}

static void test_yaml_selection_yaml_extension(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "config.yaml");
	CHECK(bcur()->syntax == syntax_find_by_name("YAML"));
	teardown();
}

static void test_yaml_selection_yml_extension(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "config.yml");
	CHECK(bcur()->syntax == syntax_find_by_name("YAML"));
	teardown();
}

static void test_yaml_selection_unrelated_extension_unchanged(void)
{
	setup(NULL);
	editor_select_syntax_highlight(bcur(), "notes.txt");
	CHECK(bcur()->syntax == NULL);
	teardown();
}

/* Keys */

/* Every registry mode resolves to its own HLDB entry, and no two entries
 * share an id: a duplicate would leave some registry id unresolvable, and
 * the loop below would see the NULL.  The synthetic modes are deliberately
 * outside HLDB and must not resolve at all. */
static void test_mode_ids_are_unique_and_resolve(void)
{
	struct editor_syntax *seen[KG_MODE_YAML + 1];
	int id, other;

	for (id = KG_MODE_C; id <= KG_MODE_YAML; id++) {
		seen[id] = syntax_find_by_mode((enum kg_mode_id)id);
		CHECK(seen[id] != NULL);
		CHECK(seen[id]->id == (enum kg_mode_id)id);
		for (other = KG_MODE_C; other < id; other++) {
			CHECK(seen[other] != seen[id]);
		}
	}

	CHECK(syntax_find_by_mode(KG_MODE_TEXT) == NULL);
	CHECK(syntax_find_by_mode(KG_MODE_IBUFFER) == NULL);
	CHECK(syntax_find_by_mode(KG_MODE_COMPILATION) == NULL);
	CHECK(syntax_find_by_mode(KG_MODE_LISP_INTERACTION) == NULL);
	CHECK(syntax_find_by_mode(KG_MODE_DIRED) == NULL);
}

/* The two lookups are two views of one table. */
static void test_mode_lookup_agrees_with_name_lookup(void)
{
	CHECK(syntax_find_by_mode(KG_MODE_C) == syntax_find_by_name("C"));
	CHECK(syntax_find_by_mode(KG_MODE_MAKEFILE)
	    == syntax_find_by_name("Makefile"));
	CHECK(syntax_find_by_mode(KG_MODE_GIT_COMMIT)
	    == syntax_find_by_name("Git commit"));
	CHECK(syntax_find_by_mode(KG_MODE_GIT_REBASE)
	    == syntax_find_by_name("Git rebase"));
	CHECK(syntax_find_by_mode(KG_MODE_YAML) == syntax_find_by_name("YAML"));
}

/* syntax_is_git_commit()/syntax_is_git_rebase() answer for the mode the
 * current buffer is in, and for no other mode -- they used to recognize
 * it by its highlighter function pointer. */
static void test_git_mode_predicates_follow_mode_id(void)
{
	setup(syntax_find_by_mode(KG_MODE_GIT_COMMIT));
	CHECK(syntax_is_git_commit());
	CHECK(!syntax_is_git_rebase());

	setup(syntax_find_by_mode(KG_MODE_GIT_REBASE));
	CHECK(!syntax_is_git_commit());
	CHECK(syntax_is_git_rebase());

	setup(syntax_find_by_mode(KG_MODE_C));
	CHECK(!syntax_is_git_commit());
	CHECK(!syntax_is_git_rebase());

	setup(NULL);
	CHECK(!syntax_is_git_commit());
	CHECK(!syntax_is_git_rebase());
	teardown();
}

/* ---- Backend syntax state (doc/plans/kg-tree-sitter-plan.md, Phase 4) ----
 *
 * A buffer carries one opaque `struct kg_syntax_state *` for whatever the
 * compiled-in backend derives from its whole text.  These tests pin the
 * lifetime rule rather than any contents, because the contents are the
 * backend's: the legacy scanners derive nothing, so every assertion below
 * is that the pointer is NULL.  That is the promise this backend makes --
 * a backend that keeps state has to break it deliberately, and these are
 * the tests that will say so. */

/* Render, prepare, adopt: the staged sequence every builder now follows.
 * The preparation pass is what colours the rows -- rendering no longer
 * does -- and every row it returns must have an hl array, because the
 * display indexes row->hl without asking.  Which colours land in it is the
 * backend's; the C block comment propagating down a staged document is
 * test_syntax_legacy.c's version of this test. */
static void test_prepare_rows_colours_and_keeps_no_state(void)
{
	struct editor_syntax *c = syntax_find_by_mode(KG_MODE_C);
	struct kg_syntax_state *st;
	erow *rows = NULL;
	int numrows = 0, cap = 0, ok = 0;

	setup(c);
	CHECK(bcur()->syntax_state == NULL);

	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "/* open", 7));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "still", 5));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "*/ int x;", 9));
	CHECK(kg_row_builder_render(rows, numrows) == 0);
	/* Rendering is rendering only now: nothing is coloured yet. */
	CHECK(rows[0].render != NULL && rows[0].hl == NULL);

	st = syntax_prepare_rows(rows, numrows, c, NULL, &ok);
	CHECK(ok == 1);
	CHECK(rows[0].hl != NULL && rows[1].hl != NULL && rows[2].hl != NULL);

	/* Whether `st` is NULL is the backend's business and is asserted in
	 * the backend's own suite: the legacy scanners keep nothing and
	 * return NULL, a parsing backend returns its parser and tree.  What
	 * is neutral, and is the whole point of the handover, is that
	 * publication releases whatever described the OLD rows and that the
	 * buffer then holds exactly what was prepared against the new ones. */
	kg_buffer_adopt_rows(bcur(), &rows, &numrows, &cap);
	CHECK(bcur()->syntax_state == NULL);
	syntax_state_adopt(bcur(), st);
	CHECK(bcur()->syntax_state == st);
	teardown();
}

/* A mode that owns its highlighter outright (dired's listing) is coloured
 * by the preparation pass too: its rows never reach a backend scanner, and
 * a staged rebuild of the listing must not come out blank. */
static void test_prepare_rows_honours_mode_owned_highlighter(void)
{
	struct editor_syntax owned
	    = { KG_MODE_DIRED, "Owned", NULL, "", dummy_highlight };
	erow *rows = NULL;
	int numrows = 0, cap = 0, ok = 0;

	setup(&owned);
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "listing", 7));
	CHECK(kg_row_builder_render(rows, numrows) == 0);
	CHECK(syntax_prepare_rows(rows, numrows, &owned, NULL, &ok) == NULL);
	CHECK(ok == 1);
	CHECK(rows[0].hl != NULL && rows[0].hl[0] == HL_KEYWORD1);
	kg_row_builder_free(&rows, &numrows, &cap);
	teardown();
}

/* Release and adopt are the whole of the ownership rule, and both have to
 * be safe on a buffer that holds no state -- which is every buffer under
 * this backend, and the first state of every buffer under any other.  The
 * release points overlap by design (killing a buffer whose rows were just
 * replaced passes two of them), so a second release must be a no-op rather
 * than a double free. */
static void test_syntax_state_release_and_adopt_are_null_safe(void)
{
	setup(NULL);
	syntax_state_release(bcur());
	CHECK(bcur()->syntax_state == NULL);
	syntax_state_release(bcur());
	CHECK(bcur()->syntax_state == NULL);
	syntax_state_adopt(bcur(), NULL);
	CHECK(bcur()->syntax_state == NULL);
	syntax_state_adopt(bcur(), NULL);
	CHECK(bcur()->syntax_state == NULL);
	syntax_state_discard(NULL); /* An abandoned load's leftover. */
	teardown();
}

/* The mode-change release point: a real change goes through it, and a
 * re-selection of the mode the buffer is already in does not.
 *
 * C# is the mode changed TO because it is one no backend builds state for
 * in either configuration: the legacy scanners keep none for anything, and
 * no tree-sitter grammar is registered for it -- /opt-9 has none, and
 * Refinement decision 4 leaves such modes as plain text.  (This used to be
 * Shell, until the plan's batch 2 turned Shell into a mode that is
 * grammarless only because the installed tree-sitter-bash is too old.)  A
 * mode a backend CAN parse acquires state at the same point, which is that
 * backend's own assertion -- test_syntax_tree_sitter.c's
 * test_mode_change_acquires_and_releases_state(). */
static void test_mode_change_releases_state(void)
{
	setup(syntax_find_by_mode(KG_MODE_C));
	editor_set_syntax(bcur(), syntax_find_by_mode(KG_MODE_CSHARP));
	CHECK(bcur()->syntax == syntax_find_by_mode(KG_MODE_CSHARP));
	CHECK(bcur()->syntax_state == NULL);
	editor_set_syntax(bcur(), syntax_find_by_mode(KG_MODE_CSHARP));
	CHECK(bcur()->syntax_state == NULL);
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_ascii_classification_is_sign_independent);
	RUN(test_syntax_to_color);
	RUN(test_gitcommit_subject_skips_comment);
	RUN(test_gitcommit_no_subject);
	RUN(test_gitrebase_pick_span);
	RUN(test_gitrebase_detect_basename_only);
	RUN(test_gitrebase_flags_end);
	RUN(test_gitcommit_detect_basename_only);
	RUN(test_lisp_el_extension_selects);
	RUN(test_lisp_fe_extension_unchanged);
	RUN(test_custom_highlighter_pointer);
	RUN(test_editor_set_syntax_rebuilds);
	RUN(test_rehighlight_all_linear_complexity);
	RUN(test_editor_set_syntax_persistence);
	RUN(test_yaml_selection_yaml_extension);
	RUN(test_yaml_selection_yml_extension);
	RUN(test_yaml_selection_unrelated_extension_unchanged);
	RUN(test_go_selection_go_extension);
	RUN(test_go_selection_go_mod_unchanged);
	RUN(test_mode_ids_are_unique_and_resolve);
	RUN(test_mode_lookup_agrees_with_name_lookup);
	RUN(test_git_mode_predicates_follow_mode_id);
	RUN(test_prepare_rows_colours_and_keeps_no_state);
	RUN(test_prepare_rows_honours_mode_owned_highlighter);
	RUN(test_syntax_state_release_and_adopt_are_null_safe);
	RUN(test_mode_change_releases_state);
	return test_summary();
}
