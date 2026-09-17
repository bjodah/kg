/* test_shindent.c — shell-script indentation: the rules, and the edits.
 *
 * The expectations are sh-mode's common cases, measured against Emacs 31
 * (sh-basic-offset 4, indent-tabs-mode t, tab-width 8): openers deepen
 * one level, closers find their opener by scanning upward so nesting
 * works, and a depth-2 indent is one TAB.  What kg deliberately does not
 * copy -- paren alignment, continuation +1, the case body's second level
 * -- is asserted as the documented divergence, so a change there fails
 * here first.
 */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/localvars.h"
#include "../src/shindent.h"
#include "../src/syntax.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setup(int shell)
{
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	bcur()->syntax = shell ? syntax_find_by_mode(KG_MODE_SHELL) : NULL;
	/* bufmgr.c's buf_reset() answers UNSET for a fresh buffer; the
	 * harness zeroes instead, which reads as nil. */
	bcur()->indent_tabs_mode_local = LOCAL_BOOL_UNSET;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	bcur()->syntax = NULL;
	undo_free();
}

static void fill(const char *const *lines, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		editor_insert_row(bcur(), i, lines[i], (int)strlen(lines[i]));
	}
	bcur()->dirty = 0;
}

/* Target of row `idx' at tab width 8, the tests' standing width. */
static int target(int idx)
{
	return shindent_target_for_rows(bcur()->row, bcur()->numrows, idx, 8);
}

static void check_row(int idx, const char *want)
{
	CHECK(bcur()->row[idx].size == (int)strlen(want));
	CHECK(memcmp(bcur()->row[idx].chars, want, strlen(want)) == 0);
}

static char *buffer_text(void)
{
	int len = 0;

	return editor_rows_to_string(bcur()->row, bcur()->numrows, &len);
}

/* ---- The reported case ---- */

/* `if [ $foo = bar ]; then' RET sits one level in: four spaces, the way
 * sh-mode's electric indent does. */
static void test_then_newline_indents_four(void)
{
	const char *lines[1] = { "if [ $foo = bar ]; then" };

	setup(1);
	fill(lines, 1);
	editor_cursor_goto(0, 23);
	editor_insert_newline();
	CHECK(bcur()->numrows == 2);
	check_row(0, "if [ $foo = bar ]; then");
	check_row(1, "    ");
	CHECK(editor_current_filerow() == 1);
	CHECK(editor_current_filecol() == 4);
	teardown();
}

/* The same RET is one undo step, like every other newline. */
static void test_then_newline_is_one_undo(void)
{
	const char *lines[1] = { "if [ $foo = bar ]; then" };
	char *text;

	setup(1);
	fill(lines, 1);
	bcur()->dirty = 0;
	editor_cursor_goto(0, 23);
	editor_insert_newline();
	CHECK(bcur()->undostack.size == 1);
	editor_undo();
	text = buffer_text();
	CHECK(strcmp(text, "if [ $foo = bar ]; then") == 0);
	free(text);
	teardown();
}

/* Outside shell buffers RET still copies the old indent. */
static void test_nonshell_newline_copies_indent(void)
{
	const char *lines[1] = { "    hello" };

	setup(0);
	fill(lines, 1);
	editor_cursor_goto(0, 9);
	editor_insert_newline();
	CHECK(bcur()->numrows == 2);
	check_row(1, "    ");
	teardown();
}

/* ---- fi TAB, flat and nested ---- */

/* Body then `fi' TAB: the closer dedents to its `if'. */
static void test_fi_tab_dedents(void)
{
	const char *lines[3]
	    = { "if [ $foo = bar ]; then", "    echo hi", "        fi" };

	setup(1);
	fill(lines, 3);
	CHECK(target(2) == 0);
	editor_cursor_goto(2, 10);
	shindent_indent_current_line();
	check_row(2, "fi");
	CHECK(editor_current_filerow() == 2);
	/* Point was past the indent, so it keeps its offset into `fi'. */
	CHECK(editor_current_filecol() == 2);
	teardown();
}

/* Nested ifs: each `fi' finds its own `if', inner at 4, outer at 0. */
static void test_nested_fi_tab_dedents(void)
{
	const char *lines[6]
	    = { "if a; then", "    if b; then", "        echo deep",
		      "        fi", "        echo out", "        fi" };

	setup(1);
	fill(lines, 6);
	CHECK(target(3) == 4);
	CHECK(target(5) == 0);
	editor_cursor_goto(3, 10);
	shindent_indent_current_line();
	check_row(3, "    fi");
	editor_cursor_goto(5, 10);
	shindent_indent_current_line();
	check_row(5, "fi");
	/* The line between the closers keeps the outer level. */
	CHECK(target(4) == 4);
	teardown();
}

/* A line after a closer sits where the closer does. */
static void test_line_after_fi_keeps_level(void)
{
	const char *lines[4]
	    = { "if a; then", "    if b; then", "        echo", "    fi" };

	setup(1);
	fill(lines, 4);
	CHECK(shindent_newline_indent(bcur()->row, 4, 3, 8) == 4);
	teardown();
}

/* An already-correct TAB is not an edit: no undo record, and point
 * inside the indent moves to its end. */
static void test_correct_tab_is_no_edit(void)
{
	const char *lines[2] = { "if a; then", "    echo" };

	setup(1);
	fill(lines, 2);
	bcur()->dirty = 0;
	editor_cursor_goto(1, 2);
	shindent_indent_current_line();
	check_row(1, "    echo");
	CHECK(editor_current_filecol() == 4);
	CHECK(bcur()->undostack.size == 0);
	teardown();
}

/* TAB keeps point's side of the indent: inside goes to its end, past
 * it keeps its offset. */
static void test_tab_keeps_point_side(void)
{
	const char *lines[2] = { "if a; then", "echo" };

	setup(1);
	fill(lines, 2);
	editor_cursor_goto(1, 0);
	shindent_indent_current_line();
	check_row(1, "    echo");
	CHECK(editor_current_filecol() == 4);
	teardown();
}

static void test_tab_past_indent_keeps_offset(void)
{
	const char *lines[2] = { "if a; then", "  echo" };

	setup(1);
	fill(lines, 2);
	editor_cursor_goto(1, 6);
	shindent_indent_current_line();
	check_row(1, "    echo");
	CHECK(editor_current_filecol() == 8);
	teardown();
}

/* ---- The other blocks ---- */

static void test_loops_dedent(void)
{
	const char *lines[9] = { "for i in 1 2; do", "    echo $i", "done",
		"while true; do", "    break", "done", "until false; do",
		"    x=1", "done" };

	setup(1);
	fill(lines, 9);
	CHECK(target(1) == 4);
	CHECK(target(2) == 0);
	CHECK(target(4) == 4);
	CHECK(target(5) == 0);
	CHECK(target(7) == 4);
	CHECK(target(8) == 0);
	teardown();
}

static void test_select_and_bare_do(void)
{
	const char *lines[4]
	    = { "select x in a b", "do", "    echo $x", "done" };

	setup(1);
	fill(lines, 4);
	CHECK(target(1) == 0);
	CHECK(target(2) == 4);
	CHECK(target(3) == 0);
	teardown();
}

static void test_bare_then_opens(void)
{
	const char *lines[4] = { "if true", "then", "    echo x", "fi" };

	setup(1);
	fill(lines, 4);
	CHECK(target(1) == 0);
	CHECK(target(2) == 4);
	CHECK(target(3) == 0);
	teardown();
}

static void test_else_and_elif(void)
{
	const char *lines[7] = { "if true; then", "    echo a",
		"elif false; then", "    echo b", "else", "    echo c", "fi" };

	setup(1);
	fill(lines, 7);
	CHECK(target(2) == 0);
	CHECK(target(3) == 4);
	CHECK(target(4) == 0);
	CHECK(target(5) == 4);
	CHECK(target(6) == 0);
	teardown();
}

static void test_braces(void)
{
	const char *lines[6] = { "foo() {", "    echo x", "}", "if a; then",
		"    {", "    echo y" };

	setup(1);
	fill(lines, 6);
	CHECK(target(1) == 4);
	CHECK(target(2) == 0);
	CHECK(target(4) == 4);
	CHECK(target(5) == 8);
	teardown();
}

/* `} else {' closes for its own line and opens for the next. */
static void test_brace_else_brace(void)
{
	const char *lines[4]
	    = { "if a; then", "    echo", "} else {", "    echo" };

	setup(1);
	fill(lines, 4);
	CHECK(target(2) == 0);
	CHECK(target(3) == 4);
	teardown();
}

static void test_case_patterns(void)
{
	const char *lines[6] = { "case $x in", "    a)", "        echo a",
		"        ;;", "    *)", "        echo b" };

	setup(1);
	fill(lines, 6);
	CHECK(target(1) == 4);
	CHECK(target(2) == 8);
	CHECK(target(3) == 8);
	CHECK(target(4) == 4);
	CHECK(target(5) == 8);
	teardown();
}

/* `;;' sits at the arm's first line even when the body went deeper. */
static void test_dangle_after_deep_arm(void)
{
	const char *lines[6] = { "if a; then", "    case $x in", "        a)",
		"            if b; then", "                echo",
		"            fi" };

	setup(1);
	fill(lines, 6);
	CHECK(target(2) == 8);
	CHECK(target(3) == 12);
	CHECK(target(4) == 16);
	CHECK(target(5) == 12);
	teardown();
}

/* `;;' keeps the body level even when the arm ended in a closer:
 * measured 12 after a `fi' at 12, 8 after an `echo' at 8. */
static void test_dangle_after_closer_keeps_level(void)
{
	const char *lines[7] = { "if a; then", "    case $x in", "        a)",
		"            if b; then", "                echo",
		"            fi", "            ;;" };

	setup(1);
	fill(lines, 7);
	CHECK(target(6) == 12);
	teardown();
}

/* A chained list deepens the next line, one level per unfinished
 * line, and the `fi' still finds its `if' straight past the chain. */
static void test_chain_continuation(void)
{
	const char *lines[5] = { "if true; then", "    foo &&",
		"        bar ||", "            baz", "    fi" };

	setup(1);
	fill(lines, 5);
	CHECK(target(2) == 8);
	CHECK(target(3) == 12);
	CHECK(target(4) == 0);
	teardown();
}

static void test_esac_dedents_to_case(void)
{
	const char *lines[5] = { "case $x in", "    a)", "        echo a",
		"        ;;", "        esac" };

	setup(1);
	fill(lines, 5);
	CHECK(target(4) == 0);
	editor_cursor_goto(4, 12);
	shindent_indent_current_line();
	check_row(4, "esac");
	teardown();
}

static void test_function_keyword(void)
{
	const char *lines[3] = { "function foo {", "    echo y", "}" };

	setup(1);
	fill(lines, 3);
	CHECK(target(1) == 4);
	CHECK(target(2) == 0);
	teardown();
}

/* One-line blocks stay flat: the opener and its closer share the
 * line, so the next line continues the level. */
static void test_one_liners_stay_flat(void)
{
	const char *lines[5] = { "if x; then y; fi",
		"for i in a; do echo; done", "foo() { bar; }",
		"case $x in a) foo;; esac", "echo after" };

	setup(1);
	fill(lines, 5);
	CHECK(target(1) == 0);
	CHECK(target(2) == 0);
	CHECK(target(3) == 0);
	CHECK(target(4) == 0);
	teardown();
}

/* Comments and blank lines inherit the surrounding indent. */
static void test_comments_and_blanks_inherit(void)
{
	const char *lines[4]
	    = { "if true; then", "    # comment", "", "    echo x" };

	setup(1);
	fill(lines, 4);
	CHECK(target(1) == 4);
	CHECK(target(2) == 4);
	CHECK(target(3) == 4);
	teardown();
}

/* A trailing `#' comment does not hide the opener before it. */
static void test_trailing_comment_keeps_opener(void)
{
	const char *lines[2] = { "if x; then # open", "" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 4);
	teardown();
}

/* A trailing backslash continues the line one level deeper. */
static void test_continuation_deepens(void)
{
	const char *lines[2] = { "echo one \\", "" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 4);
	teardown();
}

/* `$(...)' is not a case pattern: it takes the neighbour rule. */
static void test_command_substitution_is_plain(void)
{
	const char *lines[3] = { "if x; then", "    y=$(foo)", "    echo" };

	setup(1);
	fill(lines, 3);
	CHECK(target(1) == 4);
	CHECK(target(2) == 4);
	teardown();
}

/* A closer with no opener above degrades to one level up from the
 * previous line, floored at zero. */
static void test_unmatched_closer_floors(void)
{
	const char *lines[2] = { "        echo", "        fi" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 4);
	teardown();
}

static void test_blank_after_unmatched_fi(void)
{
	const char *lines[2] = { "fi", "" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 0);
	teardown();
}

/* ---- Tabs, spaces, and refusal ---- */

/* Depth 2 at tab width 8 is one TAB, the way Emacs' indent-tabs-mode
 * writes it. */
static void test_depth_two_is_a_tab(void)
{
	const char *lines[3] = { "if a; then", "    if b; then", "echo deep" };
	char buf[16];
	int n;

	setup(1);
	fill(lines, 3);
	CHECK(target(2) == 8);
	n = shindent_build_indent(buf, sizeof(buf), 8, 8, 1);
	CHECK(n == 1);
	CHECK(buf[0] == '\t');
	editor_cursor_goto(2, 0);
	shindent_indent_current_line();
	check_row(2, "\techo deep");
	CHECK(editor_current_filecol() == 1);
	teardown();
}

/* indent-tabs-mode nil writes spaces at every depth. */
static void test_no_tabs_mode_writes_spaces(void)
{
	const char *lines[3] = { "if a; then", "    if b; then", "echo deep" };
	char buf[16];
	int n;

	setup(1);
	fill(lines, 3);
	bcur()->indent_tabs_mode_local = LOCAL_BOOL_FALSE;
	n = shindent_build_indent(buf, sizeof(buf), 8, 8, 0);
	CHECK(n == 8);
	CHECK(memcmp(buf, "        ", 8) == 0);
	editor_cursor_goto(2, 0);
	shindent_indent_current_line();
	check_row(2, "        echo deep");
	teardown();
}

/* Twelve columns are a tab plus four spaces. */
static void test_build_indent_mixed(void)
{
	char buf[16];
	int n;

	n = shindent_build_indent(buf, sizeof(buf), 12, 8, 1);
	CHECK(n == 5);
	CHECK(buf[0] == '\t');
	CHECK(memcmp(buf + 1, "    ", 4) == 0);
	CHECK(shindent_build_indent(buf, 2, 12, 8, 1) == -1);
	CHECK(shindent_build_indent(NULL, 8, 4, 8, 1) == -1);
}

static void test_build_indent_small_widths(void)
{
	char buf[16];

	CHECK(shindent_build_indent(buf, sizeof(buf), 0, 8, 1) == 0);
	CHECK(shindent_build_indent(buf, sizeof(buf), 4, 8, 1) == 4);
	CHECK(memcmp(buf, "    ", 4) == 0);
	CHECK(shindent_build_indent(buf, sizeof(buf), 4, 0, 1) == 4);
}

/* A read-only buffer refuses the TAB reindent and keeps point. */
static void test_readonly_refuses_tab(void)
{
	const char *lines[2] = { "if a; then", "echo" };

	setup(1);
	fill(lines, 2);
	bcur()->readonly = 1;
	editor_cursor_goto(1, 0);
	shindent_indent_current_line();
	check_row(1, "echo");
	CHECK(editor_current_filerow() == 1);
	CHECK(editor_current_filecol() == 0);
	teardown();
}
/* Documented divergence, pinned: Emacs aligns after an unclosed `(`
 * (`    (echo sub' continues at column 5, under the `e'); kg has no
 * alignment parser and continues one level in. */
static void test_paren_alignment_divergence(void)
{
	const char *lines[3] = { "if true; then", "    (echo sub", "echo" };

	setup(1);
	fill(lines, 3);
	CHECK(target(2) == 4);
	teardown();
}

/* A zero tab width reads as the default: the clamp, not a crash. */
static void test_zero_width_reads_default(void)
{
	const char *lines[1] = { "if a; then" };

	setup(1);
	fill(lines, 1);
	CHECK(shindent_target_for_rows(bcur()->row, 1, 0, 0) == 0);
	CHECK(shindent_newline_indent(bcur()->row, 1, 0, 0) == 4);
	teardown();
}

/* A blank line between the opener and its closer: the scan walks
 * straight past it. */
static void test_blank_inside_scan(void)
{
	const char *lines[3] = { "if a; then", "", "fi" };

	setup(1);
	fill(lines, 3);
	CHECK(target(1) == 4);
	CHECK(target(2) == 0);
	teardown();
}

/* An `else' deeper in the scan is interior to its own still-open
 * block: the outer `fi' pairs past it with the outer `if'. */
static void test_else_inside_inner_block(void)
{
	const char *lines[6] = { "if a; then", "    if b; then", "    else",
		"        echo", "    fi", "fi" };

	setup(1);
	fill(lines, 6);
	CHECK(target(4) == 4);
	CHECK(target(5) == 0);
	teardown();
}

/* Broken code stays aligned rather than inventing a level: a `fi'
 * with only a pattern above it sits with the pattern.  (Emacs says
 * zero here; there is no block to be in.) */
static void test_fi_after_pattern_aligns(void)
{
	const char *lines[3] = { "case $x in", "    a)", "fi" };

	setup(1);
	fill(lines, 3);
	CHECK(target(2) == 4);
	teardown();
}

/* Nothing but blanks above: level zero, not a scan off the top. */
static void test_blank_run_before_line(void)
{
	const char *lines[2] = { "", "echo" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 0);
	teardown();
}

/* A pattern with no `case' above degrades to the neighbour rule. */
static void test_pattern_without_case(void)
{
	const char *lines[2] = { "echo hi", "a)" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 0);
	teardown();
}

/* A closer with no code above floors at zero. */
static void test_closer_without_code_above(void)
{
	const char *lines[2] = { "", "fi" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 0);
	teardown();
}

/* A `$(...)' line of its own is a command, not a pattern. */
static void test_command_substitution_line(void)
{
	const char *lines[2] = { "echo", "$(foo)" };

	setup(1);
	fill(lines, 2);
	CHECK(target(1) == 0);
	teardown();
}

/* A newline addressed past the last row clamps to it. */
static void test_newline_past_eof_clamps(void)
{
	const char *lines[1] = { "if a; then" };

	setup(1);
	fill(lines, 1);
	CHECK(shindent_newline_indent(bcur()->row, 1, 5, 8) == 4);
	teardown();
}

/* RET in an empty shell buffer splits nothing and indents nothing:
 * two empty rows, the way the copied-indent path leaves them. */
static void test_newline_empty_buffer(void)
{
	setup(1);
	editor_insert_newline();
	CHECK(bcur()->numrows == 2);
	CHECK(editor_current_filerow() == 1);
	CHECK(editor_current_filecol() == 0);
	teardown();
}

/* Documented divergence, pinned: a `case' inside a case arm defeats
 * the upward scan -- the outer `esac' aligns with the inner pattern
 * instead of the outer `case'.  (Emacs says zero.) */
static void test_nested_case_outer_esac_divergence(void)
{
	const char *lines[7] = { "case $x in", "    a)", "    case $y in",
		"        b)", "            echo", "        esac", "    esac" };

	setup(1);
	fill(lines, 7);
	CHECK(target(5) == 8);
	CHECK(target(6) == 4);
	teardown();
}

int main(void)
{
	RUN(test_then_newline_indents_four);
	RUN(test_then_newline_is_one_undo);
	RUN(test_nonshell_newline_copies_indent);
	RUN(test_fi_tab_dedents);
	RUN(test_nested_fi_tab_dedents);
	RUN(test_line_after_fi_keeps_level);
	RUN(test_correct_tab_is_no_edit);
	RUN(test_tab_keeps_point_side);
	RUN(test_tab_past_indent_keeps_offset);
	RUN(test_loops_dedent);
	RUN(test_select_and_bare_do);
	RUN(test_bare_then_opens);
	RUN(test_else_and_elif);
	RUN(test_braces);
	RUN(test_brace_else_brace);
	RUN(test_case_patterns);
	RUN(test_dangle_after_deep_arm);
	RUN(test_dangle_after_closer_keeps_level);
	RUN(test_chain_continuation);
	RUN(test_esac_dedents_to_case);
	RUN(test_function_keyword);
	RUN(test_one_liners_stay_flat);
	RUN(test_comments_and_blanks_inherit);
	RUN(test_trailing_comment_keeps_opener);
	RUN(test_continuation_deepens);
	RUN(test_command_substitution_is_plain);
	RUN(test_unmatched_closer_floors);
	RUN(test_blank_after_unmatched_fi);
	RUN(test_depth_two_is_a_tab);
	RUN(test_no_tabs_mode_writes_spaces);
	RUN(test_build_indent_mixed);
	RUN(test_build_indent_small_widths);
	RUN(test_readonly_refuses_tab);
	RUN(test_paren_alignment_divergence);
	RUN(test_zero_width_reads_default);
	RUN(test_blank_inside_scan);
	RUN(test_else_inside_inner_block);
	RUN(test_fi_after_pattern_aligns);
	RUN(test_blank_run_before_line);
	RUN(test_pattern_without_case);
	RUN(test_closer_without_code_above);
	RUN(test_command_substitution_line);
	RUN(test_newline_past_eof_clamps);
	RUN(test_newline_empty_buffer);
	RUN(test_nested_case_outer_esac_divergence);
	return test_summary();
}
