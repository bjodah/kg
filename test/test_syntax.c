/* test_syntax.c — regression tests for syntax highlighting */

#include "../src/def.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HLDB is defined in syntax.c; not declared in def.h since it is an
 * implementation detail, but tests need direct access to pick a language. */
extern struct editor_syntax HLDB[];

/* ---- Helpers ---- */

static void setup(struct editor_syntax *syn)
{
	free_all_rows();
	memset(&editor, 0, sizeof(editor));
	editor.screenrows = 24;
	editor.screencols = 80;
	bcur()->syntax = syn;
}

static void teardown(void)
{
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
}

/* ---- is_separator tests ---- */

static void test_is_separator_whitespace(void)
{
	CHECK(is_separator(' '));
	CHECK(is_separator('\t'));
	CHECK(is_separator('\n'));
}

static void test_is_separator_nul(void) { CHECK(is_separator('\0')); }

static void test_is_separator_punct(void)
{
	CHECK(is_separator('('));
	CHECK(is_separator(')'));
	CHECK(is_separator('['));
	CHECK(is_separator(']'));
	CHECK(is_separator(';'));
	CHECK(is_separator(','));
	CHECK(is_separator('+'));
	CHECK(is_separator('='));
}

static void test_is_separator_alnum_false(void)
{
	CHECK(!is_separator('a'));
	CHECK(!is_separator('Z'));
	CHECK(!is_separator('0'));
	CHECK(!is_separator('_'));
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

	/* is_separator() takes an int that callers feed a plain char from,
	 * so it must survive a negative one and answer the same either
	 * way. */
	CHECK(is_separator(' ') && is_separator('\0'));
	CHECK(!is_separator((char)0xc3) && !is_separator(0xc3));
}

/* The generic scanner marks ASCII control bytes and nothing else.  It
 * used to test isprint() on a signed char, so every byte of every
 * ordinary UTF-8 character in a C buffer came out HL_NONPRINT and was
 * then fed to a renderer that turned byte 0xDB into a raw ESC. */
static void test_generic_scan_marks_only_ascii_controls(void)
{
	int i;

	setup(syntax_find_by_name("C"));
	/* "x = \"\u00e9\"; " with an ESC and a DEL after it. */
	editor_insert_row(bcur(), 0, "int \xc3\xa9 = 1;\x1b\x7f", 13);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2); /* int */
	for (i = 4; i <= 5; i++) {
		CHECK(bcur()->row[0].hl[i] != HL_NONPRINT); /* the U+00E9 */
	}
	CHECK(bcur()->row[0].hl[9] == HL_NUMBER); /* 1 */
	CHECK(bcur()->row[0].hl[11] == HL_NONPRINT); /* ESC */
	CHECK(bcur()->row[0].hl[12] == HL_NONPRINT); /* DEL */
	teardown();
}

/* ---- editor_syntax_to_color tests ---- */

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

/* ---- C syntax tests (HLDB[0]) ---- */

/* "int x;" → "int" is a type keyword (HL_KEYWORD2, the trailing-| group). */
static void test_c_type_keyword(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "int x;", 6);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[1] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[2] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[3] == HL_NORMAL); /* space after keyword */
	teardown();
}

/* "return 0;" → "return" is a control keyword (HL_KEYWORD1). */
static void test_c_ctrl_keyword(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "return 0;", 9);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[5] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[6] == HL_NORMAL); /* space */
	CHECK(bcur()->row[0].hl[7] == HL_NUMBER); /* 0 */
	teardown();
}

/* A double-quoted string literal is HL_STRING throughout. */
static void test_c_string(void)
{
	int i;

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "\"hello\"", 7);

	for (i = 0; i < 7; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_STRING);
	}
	teardown();
}

/* A single-line comment "//" colours the rest of the line HL_COMMENT. */
static void test_c_line_comment(void)
{
	int i;

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "// comment", 10);

	for (i = 0; i < 10; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	teardown();
}

/* A decimal integer literal is HL_NUMBER. */
static void test_c_integer(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "42", 2);

	CHECK(bcur()->row[0].hl[0] == HL_NUMBER);
	CHECK(bcur()->row[0].hl[1] == HL_NUMBER);
	teardown();
}

/* A hex literal 0xff is fully HL_NUMBER. */
static void test_c_hex(void)
{
	int i;

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "0xff", 4);

	for (i = 0; i < 4; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NUMBER);
	}
	teardown();
}

/* A binary literal 0b101 is fully HL_NUMBER. */
static void test_c_binary(void)
{
	int i;

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "0b101", 5);

	for (i = 0; i < 5; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NUMBER);
	}
	teardown();
}

/* Identifiers that merely contain a keyword substring are not highlighted. */
static void test_c_no_partial_keyword(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "returning", 9); /* not "return" */

	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	teardown();
}

/* ---- Makefile syntax tests (HLDB[18]) ---- */

/* "all: src" → the target name "all" is HL_KEYWORD1. */
static void test_make_target(void)
{
	setup(syntax_find_by_name("Makefile"));
	editor_insert_row(bcur(), 0, "all: src", 8);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[1] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[2] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[3] == HL_NORMAL); /* ':' not highlighted */
	teardown();
}

/* "CC = gcc" → the variable "CC" is HL_KEYWORD2, "=" is HL_KEYWORD1. */
static void test_make_simple_assignment(void)
{
	setup(syntax_find_by_name("Makefile"));
	editor_insert_row(bcur(), 0, "CC = gcc", 8);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[1] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[2] == HL_NORMAL); /* space */
	CHECK(bcur()->row[0].hl[3] == HL_KEYWORD1); /* '=' */
	teardown();
}

/* "CFLAGS := -Wall" → ":=" is a compound assignment operator (HL_KEYWORD1). */
static void test_make_compound_assignment(void)
{
	setup(syntax_find_by_name("Makefile"));
	editor_insert_row(bcur(), 0, "CFLAGS := -Wall", 15);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2); /* C */
	CHECK(bcur()->row[0].hl[6] == HL_NORMAL); /* space */
	CHECK(bcur()->row[0].hl[7] == HL_KEYWORD1); /* ':' of ':=' */
	CHECK(bcur()->row[0].hl[8] == HL_KEYWORD1); /* '=' of ':=' */
	teardown();
}

/* "# comment" → the whole line is HL_COMMENT. */
static void test_make_comment(void)
{
	int i;

	setup(syntax_find_by_name("Makefile"));
	editor_insert_row(bcur(), 0, "# comment", 9);

	for (i = 0; i < 9; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	teardown();
}

/* ---- Markdown syntax tests (HLDB[19]) ---- */

/* "# Heading" → fully HL_KEYWORD1. */
static void test_md_atx_heading(void)
{
	int i;

	setup(syntax_find_by_name("Markdown"));
	editor_insert_row(bcur(), 0, "# Heading", 9);

	for (i = 0; i < 9; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD1);
	}
	teardown();
}

/* "> blockquote" → fully HL_COMMENT. */
static void test_md_blockquote(void)
{
	int i;

	setup(syntax_find_by_name("Markdown"));
	editor_insert_row(bcur(), 0, "> quote", 7);

	for (i = 0; i < 7; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	teardown();
}

/* ``` fence line → fully HL_STRING. */
static void test_md_fenced_code_fence(void)
{
	int i;

	setup(syntax_find_by_name("Markdown"));
	editor_insert_row(bcur(), 0, "```", 3);

	for (i = 0; i < 3; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_STRING);
	}
	teardown();
}

/* Setext heading underline "=====" → fully HL_KEYWORD1. */
static void test_md_setext_underline(void)
{
	int i;

	setup(syntax_find_by_name("Markdown"));
	/* Two rows: the heading text and the underline.
	 * markdown_syntax checks row->idx+1 to detect setext underlines,
	 * so both rows must exist with correct idx values. */
	editor_insert_row(bcur(), 0, "Hello", 5);
	editor_insert_row(bcur(), 1, "=====", 5);
	/* editor_insert_row updates syntax before incrementing numrows, so the
	 * "row above underline" re-trigger fires with numrows=1 and misses.
	 * Re-trigger row 0 now that numrows is correct. */
	editor_update_row(bcur(), &bcur()->row[0]);

	/* The underline row itself is HL_KEYWORD1. */
	for (i = 0; i < 5; i++) {
		CHECK(bcur()->row[1].hl[i] == HL_KEYWORD1);
	}

	/* The heading text row is re-highlighted as HL_KEYWORD1 too. */
	for (i = 0; i < 5; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD1);
	}
	teardown();
}

/* Regression: a lone "**" on a line used to write one past the end of
 * row->hl (heap-buffer-overflow caught by AddressSanitizer when opening
 * doc/TODO.md).  An unmatched marker must leave the row HL_NORMAL. */
static void test_md_unmatched_bold(void)
{
	int i;

	setup(syntax_find_by_name("Markdown"));
	editor_insert_row(bcur(), 0, "stray '**' marker", 17);

	for (i = 0; i < 17; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NORMAL);
	}
	teardown();
}

/* ---- Git commit syntax tests (HLDB[21]) ---- */

/* "# comment line" -> every byte HL_COMMENT. */
static void test_gitcommit_comment_line(void)
{
	int i;

	setup(syntax_find_by_name("Git commit"));
	editor_insert_row(bcur(), 0, "# comment line", 14);

	for (i = 0; i < 14; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	teardown();
}

/* A 60-char subject on row 0: bytes 0-49 HL_NORMAL, 50-59 HL_WARNING.
 * Deliberately a single insert with no re-trigger: this is a regression
 * test for a freshly opened file (editor_open() inserts each row once,
 * in order), which must show the warning without requiring an edit. */
static void test_gitcommit_subject_overflow(void)
{
	char subject[61];
	int i;

	setup(syntax_find_by_name("Git commit"));
	memset(subject, 'x', 60);
	editor_insert_row(bcur(), 0, subject, 60);

	for (i = 0; i < 50; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NORMAL);
	}
	for (i = 50; i < 60; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_WARNING);
	}
	teardown();
}

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

/* Subject row 0, blank row 1, 60-char body row 2: body has no HL_WARNING. */
static void test_gitcommit_body_not_warned(void)
{
	char body[61];
	int i;

	setup(syntax_find_by_name("Git commit"));
	memset(body, 'y', 60);
	editor_insert_row(bcur(), 0, "short subject", 13);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, body, 60);

	for (i = 0; i < 60; i++) {
		CHECK(bcur()->row[2].hl[i] == HL_NORMAL);
	}
	teardown();
}

/* A buffer of only comments has no subject and no warning anywhere. */
static void test_gitcommit_no_subject(void)
{
	int i;

	setup(syntax_find_by_name("Git commit"));
	editor_insert_row(bcur(), 0, "# only a comment", 16);
	editor_insert_row(bcur(), 1, "# another comment", 17);

	CHECK(syntax_git_commit_subject() == -1);
	for (i = 0; i < 16; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	for (i = 0; i < 17; i++) {
		CHECK(bcur()->row[1].hl[i] == HL_COMMENT);
	}
	teardown();
}

/* ---- Git rebase todo syntax tests ---- */

/* "# comment" -> every byte HL_COMMENT. */
static void test_gitrebase_comment_line(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "# Rebase abc..def onto abc", 26);

	for (i = 0; i < 26; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	teardown();
}

/* "pick 1a2b3c4 Subject": action KEYWORD1, hash KEYWORD2, rest NORMAL.
 * The subject word "decade" is all hex letters but must stay NORMAL:
 * only the first word after the action is hash-checked. */
static void test_gitrebase_pick_line(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "pick 1a2b3c4 decade", 19);

	for (i = 0; i < 4; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD1);
	}
	CHECK(bcur()->row[0].hl[4] == HL_NORMAL);
	for (i = 5; i < 12; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD2);
	}
	for (i = 12; i < 19; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NORMAL);
	}
	teardown();
}

/* A typoed action word gets HL_WARNING. */
static void test_gitrebase_unknown_action_warns(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "puck 1a2b3c4 oops", 17);

	for (i = 0; i < 4; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_WARNING);
	}
	teardown();
}

/* "exec make check": the command body is a string. */
static void test_gitrebase_exec_body_string(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "exec make check", 15);

	for (i = 0; i < 4; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD1);
	}
	for (i = 5; i < 15; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_STRING);
	}
	teardown();
}

/* Abbreviated action, and fixup's -C flag before the hash. */
static void test_gitrebase_abbrev_and_flag(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "f -C 1a2b3c4 subj", 17);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[2] == HL_NORMAL);
	CHECK(bcur()->row[0].hl[3] == HL_NORMAL);
	for (i = 5; i < 12; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD2);
	}
	teardown();
}

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

/* A flag on an action that cannot take one is warned; the hash after
 * it is still recognized. */
static void test_gitrebase_invalid_flag_warns(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "pick -C 1a2b3c4 x", 17);

	CHECK(bcur()->row[0].hl[5] == HL_WARNING);
	CHECK(bcur()->row[0].hl[6] == HL_WARNING);
	for (i = 8; i < 15; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD2);
	}
	teardown();
}

/* fixup takes only -C/-c: any other flag is warned. */
static void test_gitrebase_fixup_bad_flag_warns(void)
{
	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "fixup -X 1a2b3c4", 16);

	CHECK(bcur()->row[0].hl[6] == HL_WARNING);
	CHECK(bcur()->row[0].hl[7] == HL_WARNING);
	CHECK(bcur()->row[0].hl[9] == HL_KEYWORD2);
	teardown();
}

/* merge -C <commit> <label>: the flag is valid and the hash after it
 * is highlighted, while the label stays normal. */
static void test_gitrebase_merge_hash(void)
{
	int i;

	setup(syntax_find_by_name("Git rebase"));
	editor_insert_row(bcur(), 0, "merge -C 1a2b3c4 topic", 22);

	for (i = 0; i < 5; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD1);
	}
	CHECK(bcur()->row[0].hl[6] == HL_NORMAL);
	CHECK(bcur()->row[0].hl[7] == HL_NORMAL);
	for (i = 9; i < 16; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_KEYWORD2);
	}
	for (i = 17; i < 22; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_NORMAL);
	}
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

/* Markdown blank line inside a fence regression test */
static void test_md_blank_line_in_fence(void)
{
	setup(syntax_find_by_name("Markdown"));
	editor_insert_row(bcur(), 0, "```", 3);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "code", 4);

	CHECK(bcur()->row[0].hl[0] == HL_STRING);
	CHECK(bcur()->row[1].hl == NULL);
	CHECK(bcur()->row[2].hl[0] == HL_STRING);
	teardown();
}

static void dummy_highlight(struct editor_buffer *b, struct erow *row)
{
	(void)b;

	int i;
	for (i = 0; i < row->rsize; i++) {
		row->hl[i] = HL_KEYWORD1;
	}
}

/* Custom highlighter hook test */
static void test_custom_highlighter_pointer(void)
{
	struct editor_syntax dummy_syntax
	    = { "Dummy", NULL, NULL, "", "", "", 0, dummy_highlight };
	setup(&dummy_syntax);
	editor_insert_row(bcur(), 0, "hello", 5);
	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	teardown();
}

/* test that editor_set_syntax rebuilds highlights for existing rows */
static void test_editor_set_syntax_rebuilds(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "int x;", 6);
	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);

	editor_set_syntax(bcur(), syntax_find_by_name("Markdown"));
	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	teardown();
}

/* test iterative propagation under long stack depth */
static void test_iterative_propagation(void)
{
	int i;
	setup(syntax_find_by_name("Markdown"));

	for (i = 0; i < 500; i++) {
		editor_insert_row(bcur(), i, "some text", 9);
	}

	CHECK(bcur()->row[499].hl[0] == HL_NORMAL);

	/* Insert fence at row 0, pushing all other rows down and triggering
	 * propagation */
	editor_insert_row(bcur(), 0, "```", 3);

	CHECK(bcur()->row[0].hl[0] == HL_STRING);
	CHECK(bcur()->row[1].hl[0] == HL_STRING);
	CHECK(bcur()->row[500].hl[0] == HL_STRING);
	teardown();
}

static void test_generic_comment_across_blank_line(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "/* comment", 10);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "still comment */", 16);
	editor_insert_row(bcur(), 3, "code", 4);

	CHECK(bcur()->row[0].hl[0] == HL_MLCOMMENT);
	CHECK(bcur()->row[1].hl == NULL);
	CHECK(bcur()->row[2].hl[0] == HL_MLCOMMENT);
	CHECK(bcur()->row[3].hl[0] == HL_NORMAL);
	teardown();
}

static int custom_hl_count = 0;
static void counting_highlight(struct editor_buffer *b, struct erow *row)
{
	(void)b;

	(void)row;
	custom_hl_count++;
}

static void test_rehighlight_all_linear_complexity(void)
{
	int i;
	struct editor_syntax counting_syntax
	    = { "Counting", NULL, NULL, "", "", "", 0, counting_highlight };
	setup(&counting_syntax);

	for (i = 0; i < 200; i++) {
		editor_insert_row(bcur(), i, "text", 4);
	}

	custom_hl_count = 0;
	editor_rehighlight_all(bcur());

	CHECK(custom_hl_count == 200);
	teardown();
}

static void test_propagation_on_row_deletion(void)
{
	setup(syntax_find_by_name("Markdown"));
	editor_insert_row(bcur(), 0, "```", 3);
	editor_insert_row(bcur(), 1, "code line", 9);
	editor_insert_row(bcur(), 2, "another code line", 17);

	CHECK(bcur()->row[0].hl[0] == HL_STRING);
	CHECK(bcur()->row[1].hl[0] == HL_STRING);
	CHECK(bcur()->row[2].hl[0] == HL_STRING);

	editor_del_row(bcur(), 0);

	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	CHECK(bcur()->row[1].hl[0] == HL_NORMAL);
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

static void test_makefile_empty_row(void)
{
	setup(syntax_find_by_name("Makefile"));
	editor_insert_row(bcur(), 0, "all: src", 8);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "\tgcc main.c", 11);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[1].hl == NULL);
	CHECK(bcur()->row[2].hl[0] == HL_NORMAL);
	teardown();
}

static void test_setext_recursion_stress(void)
{
	int i;
	setup(syntax_find_by_name("Markdown"));

	for (i = 0; i < 50; i++) {
		editor_insert_row(bcur(), i, "====", 4);
	}

	CHECK(bcur()->row[49].hl[0] == HL_KEYWORD1);
	teardown();
}

/* ---- YAML syntax tests ---- */

/* Selection */

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

static void test_yaml_simple_key(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "name: value", 11);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1); /* n */
	CHECK(bcur()->row[0].hl[3] == HL_KEYWORD1); /* e */
	CHECK(bcur()->row[0].hl[4] == HL_NORMAL); /* : */
	CHECK(bcur()->row[0].hl[6] == HL_NORMAL); /* v (plain value) */
	teardown();
}

static void test_yaml_seq_item_key(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "- name: first", 13);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2); /* - */
	CHECK(bcur()->row[0].hl[2] == HL_KEYWORD1); /* n of name */
	CHECK(bcur()->row[0].hl[5] == HL_KEYWORD1); /* e of name */
	teardown();
}

static void test_yaml_quoted_key(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "\"quoted key\": value", 19);

	CHECK(bcur()->row[0].hl[0] == HL_STRING); /* opening quote */
	CHECK(bcur()->row[0].hl[11] == HL_STRING); /* closing quote */
	CHECK(bcur()->row[0].hl[12] == HL_NORMAL); /* : */
	teardown();
}

static void test_yaml_merge_key(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "<<: *base", 9);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[1] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[2] == HL_NORMAL); /* : */
	CHECK(bcur()->row[0].hl[4] == HL_KEYWORD2); /* * of alias */
	CHECK(bcur()->row[0].hl[8] == HL_KEYWORD2); /* e of base */
	teardown();
}

static void test_yaml_no_false_key_in_url(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "url: http://example.com", 23);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1); /* u */
	CHECK(bcur()->row[0].hl[2] == HL_KEYWORD1); /* l */
	CHECK(bcur()->row[0].hl[3] == HL_NORMAL); /* : (the real separator) */
	CHECK(bcur()->row[0].hl[9] == HL_NORMAL); /* : inside http:// */
	teardown();
}

static void test_yaml_no_false_key_in_time(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "time: 12:34", 11);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1); /* t */
	CHECK(bcur()->row[0].hl[3] == HL_KEYWORD1); /* e */
	CHECK(bcur()->row[0].hl[6] == HL_NORMAL); /* 1 of 12:34, not a number */
	teardown();
}

/* Comments */

static void test_yaml_full_line_comment(void)
{
	int i;

	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "# comment", 9);

	for (i = 0; i < 9; i++) {
		CHECK(bcur()->row[0].hl[i] == HL_COMMENT);
	}
	teardown();
}

static void test_yaml_trailing_comment(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "key: value  # trailing comment", 30);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1); /* key */
	CHECK(bcur()->row[0].hl[12] == HL_COMMENT); /* # */
	CHECK(bcur()->row[0].hl[29] == HL_COMMENT); /* last char */
	teardown();
}

static void test_yaml_hash_in_word_not_comment(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "literal: abc#def", 16);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1); /* literal */
	CHECK(bcur()->row[0].hl[12] == HL_NORMAL); /* # inside abc#def */
	teardown();
}

static void test_yaml_quoted_hash_not_comment(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "quoted: \"not # a comment\"", 25);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1); /* quoted */
	CHECK(bcur()->row[0].hl[13] == HL_STRING); /* # inside the string */
	teardown();
}

/* Strings */

static void test_yaml_double_quoted_escape(void)
{
	setup(syntax_find_by_name("YAML"));
	/* key: "a\"b"  (a backslash-escaped quote does not close early) */
	editor_insert_row(bcur(), 0, "key: \"a\\\"b\"", 11);

	CHECK(bcur()->row[0].hl[5] == HL_STRING); /* opening quote */
	CHECK(bcur()->row[0].hl[10] == HL_STRING); /* closing quote */
	teardown();
}

static void test_yaml_single_quoted_doubled_quote(void)
{
	setup(syntax_find_by_name("YAML"));
	/* key: 'it''s fine' */
	editor_insert_row(bcur(), 0, "key: 'it''s fine'", 17);

	CHECK(bcur()->row[0].hl[5] == HL_STRING); /* opening quote */
	CHECK(bcur()->row[0].hl[9] == HL_STRING); /* embedded '' is content */
	CHECK(bcur()->row[0].hl[16] == HL_STRING); /* closing quote */
	teardown();
}

static void test_yaml_colon_and_hash_inside_strings(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "key: 'a:b#c'", 12);

	CHECK(bcur()->row[0].hl[7] == HL_STRING); /* : inside the string */
	CHECK(bcur()->row[0].hl[9] == HL_STRING); /* # inside the string */
	teardown();
}

/* Scalars */

static void test_yaml_booleans(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "true", 4);
	editor_insert_row(bcur(), 1, "False", 5);
	editor_insert_row(bcur(), 2, "TRUE", 4);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD2);
	teardown();
}

static void test_yaml_null_and_tilde(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "null", 4);
	editor_insert_row(bcur(), 1, "~", 1);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[0] == HL_KEYWORD2);
	teardown();
}

static void test_yaml_numbers(void)
{
	static const struct {
		const char *text;
		int len;
	} cases[] = {
		{ "42", 2 },
		{ "-17", 3 },
		{ "3.14", 4 },
		{ "1.0e-6", 6 },
		{ "0x2a", 4 },
		{ "0o52", 4 },
		{ ".inf", 4 },
		{ "-.inf", 5 },
		{ ".nan", 4 },
	};
	size_t n;
	int i;

	setup(syntax_find_by_name("YAML"));
	for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		editor_insert_row(bcur(), (int)n, cases[n].text, cases[n].len);
	}
	for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		for (i = 0; i < cases[n].len; i++) {
			CHECK(bcur()->row[n].hl[i] == HL_NUMBER);
		}
	}
	teardown();
}

static void test_yaml_quoted_boolean_is_string(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "flag: \"true\"", 12);

	CHECK(bcur()->row[0].hl[6] == HL_STRING); /* opening quote */
	CHECK(bcur()->row[0].hl[10] == HL_STRING); /* e of true, still string */
	teardown();
}

/* Special tokens */

static void test_yaml_document_markers(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "---", 3);
	editor_insert_row(bcur(), 1, "...", 3);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[2] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[0] == HL_KEYWORD2);
	teardown();
}

static void test_yaml_anchor_alias_tag(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "&anchor", 7);
	editor_insert_row(bcur(), 1, "*alias", 6);
	editor_insert_row(bcur(), 2, "!tag", 4);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[6] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD2);
	teardown();
}

static void test_yaml_directive(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "%YAML 1.2", 9);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[0].hl[4] == HL_KEYWORD1); /* L of %YAML */
	teardown();
}

/* Block scalars */

static void test_yaml_literal_block(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "script: |", 9);
	editor_insert_row(bcur(), 1, "  echo first", 12);
	editor_insert_row(bcur(), 2, "next: value", 11);

	CHECK(bcur()->row[0].hl[8] == HL_KEYWORD2); /* | */
	CHECK(bcur()->row[1].hl[0] == HL_NORMAL); /* indentation */
	CHECK(bcur()->row[1].hl[2] == HL_STRING); /* echo first */
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD1); /* dedent: next is a key */
	teardown();
}

static void test_yaml_folded_block(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "notes: >", 8);
	editor_insert_row(bcur(), 1, "  folded text", 13);

	CHECK(bcur()->row[0].hl[7] == HL_KEYWORD2); /* > */
	CHECK(bcur()->row[1].hl[2] == HL_STRING);
	teardown();
}

static void test_yaml_block_chomping_modifiers(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "a: |-", 5);
	editor_insert_row(bcur(), 1, "  x", 3);
	editor_insert_row(bcur(), 2, "b: |+", 5);
	editor_insert_row(bcur(), 3, "  y", 3);

	CHECK(bcur()->row[0].hl[3] == HL_KEYWORD2); /* |- */
	CHECK(bcur()->row[0].hl[4] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[2] == HL_STRING);
	CHECK(bcur()->row[2].hl[3] == HL_KEYWORD2); /* |+ */
	CHECK(bcur()->row[3].hl[2] == HL_STRING);
	teardown();
}

static void test_yaml_block_explicit_indent(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "a: |2", 5);
	editor_insert_row(
	    bcur(), 1, "  x", 3); /* content indent == explicit 2 */

	CHECK(bcur()->row[0].hl[3] == HL_KEYWORD2);
	CHECK(bcur()->row[0].hl[4] == HL_KEYWORD2);
	CHECK(
	    bcur()->row[1].hl[2] == HL_STRING); /* content, no pending phase */
	teardown();
}

static void test_yaml_block_blank_line(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "script: |", 9);
	editor_insert_row(bcur(), 1, "  line one", 10);
	editor_insert_row(bcur(), 2, "", 0);
	editor_insert_row(bcur(), 3, "  line two", 10);
	editor_insert_row(bcur(), 4, "next: value", 11);

	CHECK(bcur()->row[1].hl[2] == HL_STRING);
	CHECK(bcur()->row[2].hl == NULL);
	CHECK(bcur()->row[3].hl[2] == HL_STRING); /* survives the blank line */
	CHECK(bcur()->row[4].hl[0] == HL_KEYWORD1); /* dedent ends the block */
	teardown();
}

static void test_yaml_block_header_insertion_repropagates(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "  echo hello", 12);
	editor_insert_row(bcur(), 1, "next: value", 11);

	CHECK(bcur()->row[0].hl[2] == HL_NORMAL);

	/* Turn the previous row's owner into a block scalar header; the
	 * following rows must be re-highlighted as block content. */
	editor_insert_row(bcur(), 0, "script: |", 9);

	CHECK(bcur()->row[0].hl[8] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[2] == HL_STRING);
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD1);
	teardown();
}

/* Safety */

static void test_yaml_lone_quote_no_crash(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "\"abc", 4);

	CHECK(bcur()->row[0].hl[0] == HL_STRING);
	CHECK(bcur()->row[0].hl[3] == HL_STRING);
	teardown();
}

static void test_yaml_lone_special_chars_no_crash(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "&", 1);
	editor_insert_row(bcur(), 1, "*", 1);
	editor_insert_row(bcur(), 2, "!", 1);
	editor_insert_row(bcur(), 3, "|", 1);
	editor_insert_row(bcur(), 4, ">", 1);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[1].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[3].hl[0] == HL_KEYWORD2);
	CHECK(bcur()->row[4].hl[0] == HL_KEYWORD2);
	teardown();
}

static void test_yaml_colon_at_end_of_short_row(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, ":", 1);
	editor_insert_row(bcur(), 1, "a:", 2);

	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	CHECK(bcur()->row[1].hl[0] == HL_KEYWORD1);
	teardown();
}

static void test_yaml_empty_rows_no_crash(void)
{
	setup(syntax_find_by_name("YAML"));
	editor_insert_row(bcur(), 0, "key: value", 10);
	editor_insert_row(bcur(), 1, "", 0);
	editor_insert_row(bcur(), 2, "other: value", 12);

	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD1);
	CHECK(bcur()->row[1].hl == NULL);
	CHECK(bcur()->row[2].hl[0] == HL_KEYWORD1);
	teardown();
}

static void test_yaml_malformed_escape_at_eol_no_crash(void)
{
	setup(syntax_find_by_name("YAML"));
	/* key: "abc\  (a trailing, unterminated backslash escape) */
	editor_insert_row(bcur(), 0, "key: \"abc\\", 10);

	CHECK(bcur()->row[0].hl[5] == HL_STRING);
	CHECK(bcur()->row[0].hl[9] == HL_STRING);
	teardown();
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_is_separator_whitespace);
	RUN(test_is_separator_nul);
	RUN(test_is_separator_punct);
	RUN(test_is_separator_alnum_false);
	RUN(test_ascii_classification_is_sign_independent);
	RUN(test_generic_scan_marks_only_ascii_controls);
	RUN(test_syntax_to_color);
	RUN(test_c_type_keyword);
	RUN(test_c_ctrl_keyword);
	RUN(test_c_string);
	RUN(test_c_line_comment);
	RUN(test_c_integer);
	RUN(test_c_hex);
	RUN(test_c_binary);
	RUN(test_c_no_partial_keyword);
	RUN(test_make_target);
	RUN(test_make_simple_assignment);
	RUN(test_make_compound_assignment);
	RUN(test_make_comment);
	RUN(test_md_atx_heading);
	RUN(test_md_blockquote);
	RUN(test_md_fenced_code_fence);
	RUN(test_md_setext_underline);
	RUN(test_md_unmatched_bold);
	RUN(test_gitcommit_comment_line);
	RUN(test_gitcommit_subject_overflow);
	RUN(test_gitcommit_subject_skips_comment);
	RUN(test_gitcommit_body_not_warned);
	RUN(test_gitcommit_no_subject);
	RUN(test_gitrebase_comment_line);
	RUN(test_gitrebase_pick_line);
	RUN(test_gitrebase_unknown_action_warns);
	RUN(test_gitrebase_exec_body_string);
	RUN(test_gitrebase_abbrev_and_flag);
	RUN(test_gitrebase_pick_span);
	RUN(test_gitrebase_detect_basename_only);
	RUN(test_gitrebase_invalid_flag_warns);
	RUN(test_gitrebase_fixup_bad_flag_warns);
	RUN(test_gitrebase_merge_hash);
	RUN(test_gitrebase_flags_end);
	RUN(test_gitcommit_detect_basename_only);
	RUN(test_md_blank_line_in_fence);
	RUN(test_custom_highlighter_pointer);
	RUN(test_editor_set_syntax_rebuilds);
	RUN(test_iterative_propagation);
	RUN(test_generic_comment_across_blank_line);
	RUN(test_rehighlight_all_linear_complexity);
	RUN(test_propagation_on_row_deletion);
	RUN(test_editor_set_syntax_persistence);
	RUN(test_makefile_empty_row);
	RUN(test_setext_recursion_stress);
	RUN(test_yaml_selection_yaml_extension);
	RUN(test_yaml_selection_yml_extension);
	RUN(test_yaml_selection_unrelated_extension_unchanged);
	RUN(test_yaml_simple_key);
	RUN(test_yaml_seq_item_key);
	RUN(test_yaml_quoted_key);
	RUN(test_yaml_merge_key);
	RUN(test_yaml_no_false_key_in_url);
	RUN(test_yaml_no_false_key_in_time);
	RUN(test_yaml_full_line_comment);
	RUN(test_yaml_trailing_comment);
	RUN(test_yaml_hash_in_word_not_comment);
	RUN(test_yaml_quoted_hash_not_comment);
	RUN(test_yaml_double_quoted_escape);
	RUN(test_yaml_single_quoted_doubled_quote);
	RUN(test_yaml_colon_and_hash_inside_strings);
	RUN(test_yaml_booleans);
	RUN(test_yaml_null_and_tilde);
	RUN(test_yaml_numbers);
	RUN(test_yaml_quoted_boolean_is_string);
	RUN(test_yaml_document_markers);
	RUN(test_yaml_anchor_alias_tag);
	RUN(test_yaml_directive);
	RUN(test_yaml_literal_block);
	RUN(test_yaml_folded_block);
	RUN(test_yaml_block_chomping_modifiers);
	RUN(test_yaml_block_explicit_indent);
	RUN(test_yaml_block_blank_line);
	RUN(test_yaml_block_header_insertion_repropagates);
	RUN(test_yaml_lone_quote_no_crash);
	RUN(test_yaml_lone_special_chars_no_crash);
	RUN(test_yaml_colon_at_end_of_short_row);
	RUN(test_yaml_empty_rows_no_crash);
	RUN(test_yaml_malformed_escape_at_eol_no_crash);
	return test_summary();
}
