/* test_syntax_legacy.c -- regression tests for the legacy scanner backend
 *
 * Everything src/syntax_legacy.c decides: the generic keyword/comment/
 * string/number scan, the Markdown, Makefile, YAML, git-commit and
 * git-rebase scanners, and the cross-row hl_oc state they propagate.
 * Split out of test_syntax.c, unchanged, when the scanners moved behind
 * src/syntax_backend.h -- a build that installs a different backend does
 * not build this suite (the Makefile adds it to TESTBINS only when
 * WITH_TREE_SITTER=0).  It is not a museum: the legacy backend is
 * permanent, being the configuration that needs no dependency at all
 * (doc/plans/kg-tree-sitter-plan.md, Refinement decision 3).  The
 * backend-neutral half (mode identity, selection, colours, the mode-owned
 * highlighter hook, syntax-state lifetime) is in test/test_syntax.c. */

#include "../src/def.h"
#include "../src/edit.h"
#include "../src/syntax.h"
#include "../src/syntax_legacy.h"
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

/* is_separator() takes an int that callers feed a plain char from, so it
 * must survive a negative one and answer the same either way -- the
 * counterpart of test_syntax.c's ASCII-helper case, which
 * .ci/ci-11-char-signedness.sh runs under both signednesses. */
static void test_is_separator_negative_char(void)
{
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

/* An unterminated string whose last render byte is a lone trailing
 * backslash used to write row->hl[row->rsize] -- one byte past hl's
 * allocation, since row_hl_reserve() sizes hl to exactly rsize for rows
 * under KG_ROW_SLACK_MIN.  The C highlighter's sibling of the guard
 * test_yaml_malformed_escape_at_eol_no_crash() pins for YAML. */
static void test_c_unterminated_string_trailing_backslash_no_crash(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "\"abc\\", 5);

	CHECK(bcur()->row[0].rsize == 5);
	CHECK(bcur()->row[0].hl[0] == HL_STRING);
	CHECK(bcur()->row[0].hl[4] == HL_STRING);
	teardown();
}

/* Same shape through the Shell highlighter, which is how the overflow was
 * reached in the wild: a shell line continuing a double-quoted argument
 * onto the next row, with no closing quote anywhere in the file. */
static void test_shell_unterminated_string_trailing_backslash_no_crash(void)
{
	setup(syntax_find_by_name("Shell"));
	editor_insert_row(bcur(), 0, "x=\"export FOO=bar \\", 19);

	CHECK(bcur()->row[0].rsize == 19);
	CHECK(bcur()->row[0].hl[2] == HL_STRING);
	CHECK(bcur()->row[0].hl[18] == HL_STRING);
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

/* row->hl is indexed in render bytes, so a comment run has to end at
 * row->rsize.  With a leading tab the chars length is eight bytes
 * shorter, and the tail of the comment used to keep the colour of
 * whatever was there before. */
static void test_c_line_comment_after_tab(void)
{
	int i;

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "\t// comment", 11);

	CHECK(bcur()->row[0].size == 11);
	CHECK(bcur()->row[0].rsize == 18);
	for (i = 8; i < 18; i++) {
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

/* ---- Makefile syntax tests (KG_MODE_MAKEFILE) ---- */

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

/* ---- Markdown syntax tests (KG_MODE_MARKDOWN) ---- */

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

/* ---- Git commit syntax tests (KG_MODE_GIT_COMMIT) ---- */

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

/* ---- Edit transaction vs. from-scratch rehighlight ---- */

/* One edit-transaction differential case: build `lines` in `mode`,
 * replace the text between (r0,c0) and (r1,c1) -- chars space, per
 * doc/coordinates.md -- with `replacement`, and compare what the edit
 * left behind against what a rehighlight of the same final text
 * produces. */
struct edit_case {
	const char *mode;
	const char *const *lines;
	int nlines;
	int r0, c0, r1, c1;
	const char *replacement;
};

/* Since kg_buffer_replace() tells the syntax layer about an edit exactly
 * once (src/syntax.h's syntax_after_edit()), rather than re-running the
 * highlighter per rebuilt row, the incremental answer has to be the same
 * one a whole-buffer pass computes: same hl bytes, same cross-row hl_oc,
 * row for row.  Multi-row constructs are the whole of the risk -- a C
 * block comment, a Markdown fence and a YAML block scalar each carry
 * state across rows in hl_oc -- so each case edits across such a
 * construct's boundary, where an edit changes what the rows below it
 * mean.  This is the differential pattern the tree-sitter plan's Phase 10
 * wants between backends, applied to the one backend that exists. */
static void check_edit_matches_rehighlight(const struct edit_case *c)
{
	unsigned char *hl;
	struct kg_edit e;
	int *oc;
	int i, rows;
	size_t begin, end, total, off;

	setup(syntax_find_by_name(c->mode));
	for (i = 0; i < c->nlines; i++) {
		editor_insert_row(bcur(), i, c->lines[i], strlen(c->lines[i]));
	}
	begin = buffer_row_col_to_position(bcur(), c->r0, c->c0);
	end = buffer_row_col_to_position(bcur(), c->r1, c->c1);
	e = kg_edit_user(
	    bcur(), begin, end, c->replacement, strlen(c->replacement));
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	/* Snapshot the edit's hl into one flat arena (row offsets are
	 * re-derivable: a rebuild recolours but never resizes a render), so
	 * the cleanup is two unconditional frees whatever path runs. */
	rows = bcur()->numrows;
	total = 0;
	for (i = 0; i < rows; i++) {
		total += (size_t)bcur()->row[i].rsize;
	}
	hl = malloc(total + 1);
	oc = calloc((size_t)rows, sizeof(*oc));
	CHECK(hl != NULL && oc != NULL);
	if (!hl || !oc) {
		free(hl);
		free(oc);
		teardown();
		return;
	}
	off = 0;
	for (i = 0; i < rows; i++) {
		erow *r = &bcur()->row[i];

		oc[i] = r->hl_oc;
		if (r->rsize > 0) {
			memcpy(hl + off, r->hl, (size_t)r->rsize);
			off += (size_t)r->rsize;
		}
	}
	/* "Everything changed", which for this backend is the from-scratch
	 * pass: the same rows, recoloured with no history at all. */
	syntax_rebuild(bcur());
	off = 0;
	for (i = 0; i < rows; i++) {
		erow *r = &bcur()->row[i];

		CHECKF(oc[i] == r->hl_oc,
		    "%s row %d: hl_oc %d after the edit, "
		    "%d from scratch",
		    c->mode, i, oc[i], r->hl_oc);
		if (r->rsize > 0) {
			CHECKF(memcmp(hl + off, r->hl, (size_t)r->rsize) == 0,
			    "%s row %d (%s): hl differs from a from-scratch "
			    "rehighlight",
			    c->mode, i, r->chars);
			off += (size_t)r->rsize;
		}
	}
	free(hl);
	free(oc);
	teardown();
}

/* A C block comment that spans rows, with the edit removing the row that
 * opened it -- so the rows below stop being comment and the row that used
 * to close it stops closing anything. */
static void test_edit_differential_c_block_comment(void)
{
	static const char *const lines[] = {
		"int a = 0;",
		"/* opened here",
		"still inside the comment",
		"*/ int b = 1;",
		"int c = 2;",
	};
	const struct edit_case c = {
		.mode = "C",
		.lines = lines,
		.nlines = 5,
		.r0 = 1,
		.c0 = 0,
		.r1 = 1,
		.c1 = 14,
		.replacement = "int d = 3;\nint e = 4;",
	};

	check_edit_matches_rehighlight(&c);
}

/* The other direction: an edit that OPENS a block comment where there was
 * none, so the rows below it become comment. */
static void test_edit_differential_c_opens_comment(void)
{
	static const char *const lines[] = {
		"int a = 0;",
		"int b = 1;",
		"int c = 2;",
		"int d = 3;",
	};
	const struct edit_case c = {
		.mode = "C",
		.lines = lines,
		.nlines = 4,
		.r0 = 0,
		.c0 = 4,
		.r1 = 1,
		.c1 = 4,
		.replacement = "x /* opened\nand still open\ny",
	};

	check_edit_matches_rehighlight(&c);
}

/* A Markdown fenced block, with the edit moving the opening fence down a
 * row: every row of the block changes meaning, and so does the row that
 * used to close it. */
static void test_edit_differential_markdown_fence(void)
{
	static const char *const lines[] = {
		"# Title",
		"```",
		"fenced line",
		"another fenced line",
		"```",
		"after the fence",
	};
	const struct edit_case c = {
		.mode = "Markdown",
		.lines = lines,
		.nlines = 6,
		.r0 = 1,
		.c0 = 0,
		.r1 = 1,
		.c1 = 3,
		.replacement = "prose first\n```",
	};

	check_edit_matches_rehighlight(&c);
}

/* A YAML block scalar, with the edit replacing its header by a plain key
 * and a new header one row down. */
static void test_edit_differential_yaml_block_scalar(void)
{
	static const char *const lines[] = {
		"first: value",
		"script: |",
		"  echo one",
		"  echo two",
		"next: value",
	};
	const struct edit_case c = {
		.mode = "YAML",
		.lines = lines,
		.nlines = 5,
		.r0 = 1,
		.c0 = 0,
		.r1 = 1,
		.c1 = 9,
		.replacement = "plain: value\nscript: >",
	};

	check_edit_matches_rehighlight(&c);
}

/* Regression, and the one behaviour this slice deliberately changed: an
 * edit that stops a construct from opening must not leave the rows below
 * it coloured for it.  Rebuilding row by row used to decide whether to
 * propagate by comparing a staged row's hl_oc -- which describes the
 * staging, not the text -- and so left "text in comment" cyan under a row
 * that no longer opened a comment.  syntax_after_edit() always re-examines
 * the first row below the edit, which is what makes the differential
 * cases above hold. */
static void test_edit_clears_stale_comment_below(void)
{
	struct kg_edit e;

	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "/* open", 7);
	editor_insert_row(bcur(), 1, "text in comment", 15);
	CHECK(bcur()->row[1].hl[0] == HL_MLCOMMENT);

	e = kg_edit_user(bcur(), 0, 7, "a\nb", 3);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	CHECK(bcur()->numrows == 3);
	CHECK(bcur()->row[2].hl[0] == HL_NORMAL);
	CHECK(bcur()->row[2].hl_oc == 0);
	teardown();
}

/* ---- Staged preparation ---- */

/* The preparation pass colours a whole staged document in one go, and a
 * cross-row construct -- an open block comment -- has to propagate down it
 * exactly as it did when rendering and highlighting were interleaved one
 * row at a time.  test_syntax.c holds the facade's half of this: that the
 * pass runs, keeps no state, and leaves every row with an hl array. */
static void test_prepare_rows_propagates_block_comment(void)
{
	struct editor_syntax *c = syntax_find_by_name("C");
	erow *rows = NULL;
	int numrows = 0, cap = 0, ok = 0;

	setup(c);
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "/* open", 7));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "still", 5));
	CHECK(kg_row_builder_add_line(&rows, &numrows, &cap, "*/ int x;", 9));
	CHECK(kg_row_builder_render(rows, numrows) == 0);

	CHECK(syntax_prepare_rows(rows, numrows, c, &ok) == NULL);
	CHECK(ok == 1);
	CHECK(rows[0].hl != NULL && rows[0].hl[0] == HL_MLCOMMENT);
	CHECK(rows[1].hl != NULL && rows[1].hl[0] == HL_MLCOMMENT);
	CHECK(rows[2].hl != NULL && rows[2].hl[0] == HL_MLCOMMENT);
	CHECK(rows[0].hl_oc == 1 && rows[1].hl_oc == 1);
	CHECK(rows[2].hl_oc == 0); /* The comment closes on the last row. */
	kg_row_builder_free(&rows, &numrows, &cap);
	teardown();
}

/* editor_set_syntax() rehighlights the rows the buffer already holds, and
 * through the scanners that means a C keyword loses its colour when the
 * buffer becomes Markdown.  test_syntax.c pins the same rebuild with two
 * mode-owned highlighters, which is the part no backend can change. */
static void test_set_syntax_rebuilds_scanned_rows(void)
{
	setup(syntax_find_by_name("C"));
	editor_insert_row(bcur(), 0, "int x;", 6);
	CHECK(bcur()->row[0].hl[0] == HL_KEYWORD2);

	editor_set_syntax(bcur(), syntax_find_by_name("Markdown"));
	CHECK(bcur()->row[0].hl[0] == HL_NORMAL);
	teardown();
}

/* ---- Mode identity ---- */

int main(void)
{
	RUN(test_is_separator_whitespace);
	RUN(test_is_separator_nul);
	RUN(test_is_separator_punct);
	RUN(test_is_separator_alnum_false);
	RUN(test_is_separator_negative_char);
	RUN(test_generic_scan_marks_only_ascii_controls);
	RUN(test_c_type_keyword);
	RUN(test_c_ctrl_keyword);
	RUN(test_c_string);
	RUN(test_c_unterminated_string_trailing_backslash_no_crash);
	RUN(test_shell_unterminated_string_trailing_backslash_no_crash);
	RUN(test_c_line_comment);
	RUN(test_c_line_comment_after_tab);
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
	RUN(test_gitcommit_body_not_warned);
	RUN(test_gitrebase_comment_line);
	RUN(test_gitrebase_pick_line);
	RUN(test_gitrebase_unknown_action_warns);
	RUN(test_gitrebase_exec_body_string);
	RUN(test_gitrebase_abbrev_and_flag);
	RUN(test_gitrebase_invalid_flag_warns);
	RUN(test_gitrebase_fixup_bad_flag_warns);
	RUN(test_gitrebase_merge_hash);
	RUN(test_md_blank_line_in_fence);
	RUN(test_iterative_propagation);
	RUN(test_generic_comment_across_blank_line);
	RUN(test_propagation_on_row_deletion);
	RUN(test_makefile_empty_row);
	RUN(test_setext_recursion_stress);
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
	RUN(test_edit_differential_c_block_comment);
	RUN(test_edit_differential_c_opens_comment);
	RUN(test_edit_differential_markdown_fence);
	RUN(test_edit_differential_yaml_block_scalar);
	RUN(test_edit_clears_stale_comment_below);
	RUN(test_prepare_rows_propagates_block_comment);
	RUN(test_set_syntax_rebuilds_scanned_rows);
	return test_summary();
}
