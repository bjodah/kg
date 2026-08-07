#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../src/def.h"
#include "../src/localvars.h"
#include "test.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct local_settings make_settings(void)
{
	struct local_settings s;

	local_settings_init(&s);
	return s;
}

static void setup_one_row(erow *row, const char *text)
{
	memset(row, 0, sizeof(*row));
	row->chars = (char *)text;
	row->size = (int)strlen(text);
}

static void test_compile_command_only(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- compile-command: \"make\" -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make") == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
}

static void test_read_only_only(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- buffer-read-only: t -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.compile_command_set == false);
}

static void test_both_variables(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0],
	    "-*- buffer-read-only: t; compile-command: \"make test\" -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make test") == 0);
}

static void test_extra_whitespace(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*-   buffer-read-only :  t   -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_semicolon_inside_quoted_command(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- compile-command: \"make a; b\" -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make a; b") == 0);
}

static void test_escaped_quote_inside_command(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(
	    &rows[0], "-*- compile-command: \"printf \\\"hi\\\"\" -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "printf \"hi\"") == 0);
}

static void test_first_line_comment_prefix(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "/* -*- compile-command: \"make\" -*- */");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make") == 0);
}

static void test_shebang_second_line_modeline(void)
{
	erow rows[2];
	struct local_settings s;

	setup_one_row(&rows[0], "#!/bin/sh");
	setup_one_row(&rows[1], "# -*- compile-command: \"./test.sh\"; -*-");
	CHECK(localvars_parse_modeline(rows, 2, &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "./test.sh") == 0);
}

static void test_unterminated_modeline(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- compile-command: \"make\"");
	CHECK(localvars_parse_modeline(rows, 1, &s) != 0);
	CHECK(s.compile_command_set == false);
}

static void test_unterminated_string(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- compile-command: \"make -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.compile_command_set == false);
	CHECK(s.malformed_entries >= 1);
}

static void test_unsupported_var_before_supported(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- mode: c; compile-command: \"make\" -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.ignored_entries >= 1);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make") == 0);
}

static void test_duplicate_last_wins(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(
	    &rows[0], "-*- compile-command: \"A\"; compile-command: \"B\" -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "B") == 0);
}

static void test_buffer_read_only_nil(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- buffer-read-only: nil -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_FALSE);
}

/* A `;` inside parentheses is part of a value, not a segment separator:
 * the property list keeps scanning to the real one. */
static void test_modeline_paren_hides_semicolon(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- mode: (c ; x); buffer-read-only: t -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.ignored_entries == 1);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

/* A segment holding nothing but blanks is not an entry and not an error;
 * only a non-blank segment with no name/value colon is malformed. */
static void test_modeline_blank_segments_are_not_errors(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*- ;  ; buffer-read-only: t ;  -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.malformed_entries == 0);
	CHECK(s.ignored_entries == 0);
}

static void test_modeline_empty_name_is_malformed(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*-  : t; nocolonhere -*-");
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.malformed_entries == 2);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
}

/* The closing marker needs three bytes of its own: two left over after
 * the opening one cannot hold it. */
static void test_modeline_close_marker_needs_three_bytes(void)
{
	erow rows[1];
	struct local_settings s;

	setup_one_row(&rows[0], "-*--");
	CHECK(localvars_parse_modeline(rows, 1, &s) != 0);
	CHECK(s.compile_command_set == false);
}

/* A name longer than the parser's buffer is truncated, which can only
 * turn a match into a miss -- never into a different variable. */
static void test_modeline_overlong_name_is_ignored(void)
{
	erow rows[1];
	struct local_settings s;
	char line[256];

	snprintf(line, sizeof(line), "-*- %0*d-command: \"make\" -*-", 80, 0);
	setup_one_row(&rows[0], line);
	CHECK(localvars_parse_modeline(rows, 1, &s) == 0);
	CHECK(s.ignored_entries == 1);
	CHECK(s.compile_command_set == false);
}

static void test_merge_unset_source_leaves_destination_intact(void)
{
	struct local_settings d = make_settings();
	struct local_settings s = make_settings();

	s.compile_command_set = false;
	s.buffer_read_only = LOCAL_BOOL_UNSET;
	d.compile_command_set = true;
	strcpy(d.compile_command, "make");
	d.buffer_read_only = LOCAL_BOOL_TRUE;
	d.ignored_entries = 3;
	d.malformed_entries = 1;

	local_settings_merge(&d, &s);

	CHECK(d.compile_command_set == true);
	CHECK(strcmp(d.compile_command, "make") == 0);
	CHECK(d.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(d.ignored_entries == 3);
	CHECK(d.malformed_entries == 1);
}

static void test_merge_set_compile_command_overwrites_destination(void)
{
	struct local_settings d = make_settings();
	struct local_settings s = make_settings();

	strcpy(d.compile_command, "old");
	d.compile_command_set = true;
	strcpy(s.compile_command, "new");
	s.compile_command_set = true;

	local_settings_merge(&d, &s);

	CHECK(d.compile_command_set == true);
	CHECK(strcmp(d.compile_command, "new") == 0);
}

static void test_merge_set_ro_overwrites_unset_ro_leaves(void)
{
	struct local_settings d = make_settings();
	struct local_settings s = make_settings();

	d.buffer_read_only = LOCAL_BOOL_TRUE;
	s.buffer_read_only = LOCAL_BOOL_FALSE;
	s.compile_command_set = true;
	strcpy(s.compile_command, "cmd");

	local_settings_merge(&d, &s);

	CHECK(d.buffer_read_only == LOCAL_BOOL_FALSE);
	CHECK(d.compile_command_set == true);
	CHECK(strcmp(d.compile_command, "cmd") == 0);

	{
		struct local_settings d2 = make_settings();
		struct local_settings s2 = make_settings();

		d2.buffer_read_only = LOCAL_BOOL_TRUE;
		s2.buffer_read_only = LOCAL_BOOL_UNSET;

		local_settings_merge(&d2, &s2);
		CHECK(d2.buffer_read_only == LOCAL_BOOL_TRUE);
	}
}

static void test_merge_counters_accumulate(void)
{
	struct local_settings d = make_settings();
	struct local_settings s = make_settings();

	d.ignored_entries = 2;
	d.malformed_entries = 1;
	s.ignored_entries = 3;
	s.malformed_entries = 4;

	local_settings_merge(&d, &s);

	CHECK(d.ignored_entries == 5);
	CHECK(d.malformed_entries == 5);
}

struct row_builder {
	erow *arr;
	int cap;
	int n;
	int fixed;
	erow fixed_buf[32];
};

static void rb_init(struct row_builder *rb, int nlines)
{
	rb->n = 0;
	if (nlines <= 32) {
		rb->arr = rb->fixed_buf;
		rb->cap = 32;
		rb->fixed = 1;
	} else {
		rb->arr = NULL;
		rb->cap = 0;
		rb->fixed = 0;
	}
}

static void rb_add(struct row_builder *rb, const char *text)
{
	if (!rb->arr) {
		return;
	}
	if (rb->n >= rb->cap) {
		return;
	}
	memset(&rb->arr[rb->n], 0, sizeof(erow));
	rb->arr[rb->n].chars = (char *)text;
	rb->arr[rb->n].size = (int)strlen(text);
	rb->n++;
}

static void rb_free(struct row_builder *rb)
{
	rb->arr = NULL;
	rb->n = 0;
}

static struct local_settings parse_footer_lines(const char **src, int nlines)
{
	struct row_builder rb;
	struct local_settings s;

	rb_init(&rb, nlines);
	for (int i = 0; i < nlines; i++) {
		rb_add(&rb, src[i]);
	}
	localvars_parse_footer(rb.arr, rb.n, &s);
	rb_free(&rb);
	return s;
}

static void test_footer_hash_comment_ro(void)
{
	const char *lines[] = {
		"# Local Variables:",
		"# buffer-read-only: t",
		"# End:",
	};
	struct local_settings s;

	s = parse_footer_lines(lines, 3);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.malformed_entries == 0);
}

static void test_footer_c_block_ro(void)
{
	const char *lines[] = {
		"/* Local Variables: */",
		"/* buffer-read-only: t */",
		"/* End: */",
	};
	struct local_settings s;

	s = parse_footer_lines(lines, 3);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.malformed_entries == 0);
}

static void test_footer_no_prefix_ro(void)
{
	const char *lines[] = {
		"Local Variables:",
		"buffer-read-only: t",
		"End:",
	};
	struct local_settings s;

	s = parse_footer_lines(lines, 3);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.malformed_entries == 0);
}

static void test_footer_case_variants(void)
{
	const char *lines[] = {
		"# local VARIABLES:",
		"# buffer-read-only: t",
		"# End:",
	};
	struct local_settings s;

	s = parse_footer_lines(lines, 3);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.malformed_entries == 0);
}

static void test_footer_missing_end(void)
{
	const char *lines[] = {
		"# Local Variables:",
		"# buffer-read-only: t",
	};
	struct local_settings s;
	int rc;

	s = make_settings();
	{
		struct row_builder rb;

		rb_init(&rb, 2);
		for (int i = 0; i < 2; i++) {
			rb_add(&rb, lines[i]);
		}
		rc = localvars_parse_footer(rb.arr, rb.n, &s);
		rb_free(&rb);
	}
	CHECK(rc != 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
	CHECK(s.compile_command_set == false);
}

static void test_footer_outside_3000_bytes(void)
{
#define PAD_BYTES 3100
	static char pad[PAD_BYTES + 1];
	const char *lines[5];
	erow rows[5];
	struct local_settings s;
	int rc;

	if (pad[0] == '\0') {
		memset(pad, 'x', PAD_BYTES);
		pad[PAD_BYTES] = '\0';
	}
	lines[0] = "# Local Variables:";
	lines[1] = "# buffer-read-only: t";
	lines[2] = "# End:";
	lines[3] = pad;
	lines[4] = "trailing content";

	for (int i = 0; i < 5; i++) {
		memset(&rows[i], 0, sizeof(erow));
		rows[i].chars = (char *)lines[i];
		rows[i].size = (int)strlen(lines[i]);
	}

	s = make_settings();
	rc = localvars_parse_footer(rows, 5, &s);
	CHECK(rc != 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
	CHECK(s.compile_command_set == false);
#undef PAD_BYTES
}

static void test_footer_form_feed_excludes_early_block(void)
{
	const char *lines[] = {
		"# Local Variables:",
		"# buffer-read-only: t",
		"# End:",
		"\f",
		"trailing content without marker",
	};
	struct local_settings s;
	int rc;

	s = make_settings();
	{
		struct row_builder rb;

		rb_init(&rb, 5);
		for (int i = 0; i < 5; i++) {
			rb_add(&rb, lines[i]);
		}
		rc = localvars_parse_footer(rb.arr, rb.n, &s);
		rb_free(&rb);
	}
	CHECK(rc != 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);

	{
		const char *lines2[] = {
			"old block before ff",
			"\f",
			"# Local Variables:",
			"# buffer-read-only: t",
			"# End:",
		};
		struct local_settings s2;

		s2 = parse_footer_lines(lines2, 5);
		CHECK(s2.buffer_read_only == LOCAL_BOOL_TRUE);
	}
}

static void test_footer_continued_compile_command(void)
{
	const char *lines[] = {
		"# Local Variables:",
		"# compile-command: \"cc foo.c \\",
		"#   -o foo\"",
		"# End:",
	};
	struct local_settings s;

	s = parse_footer_lines(lines, 4);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "cc foo.c   -o foo") == 0);
	CHECK(s.malformed_entries == 0);
}

static void test_footer_override_modeline(void)
{
	erow modeline_row[1];
	struct local_settings ml, foot, merged;

	setup_one_row(&modeline_row[0], "-*- compile-command: \"OLD\" -*-");
	localvars_parse_modeline(modeline_row, 1, &ml);
	CHECK(ml.compile_command_set == true);
	CHECK(strcmp(ml.compile_command, "OLD") == 0);

	{
		const char *footer_lines[] = {
			"# Local Variables:",
			"# compile-command: \"NEW\"",
			"# End:",
		};

		foot = parse_footer_lines(footer_lines, 3);
		CHECK(foot.compile_command_set == true);
		CHECK(strcmp(foot.compile_command, "NEW") == 0);
	}

	merged = ml;
	local_settings_merge(&merged, &foot);
	CHECK(merged.compile_command_set == true);
	CHECK(strcmp(merged.compile_command, "NEW") == 0);
}

static void test_footer_invalid_ro_value(void)
{
	const char *lines[] = {
		"# Local Variables:",
		"# buffer-read-only: yes",
		"# End:",
	};
	struct local_settings s;

	s = parse_footer_lines(lines, 3);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
	CHECK(s.malformed_entries >= 1);
}

/* ---- .dir-locals.el parse tests ---- */

static void test_dl_read_only_t(void)
{
	const char *src = "((nil . ((buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.compile_command_set == false);
}

static void test_dl_compile_command(void)
{
	const char *src = "((nil . ((compile-command . \"make test\"))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make test") == 0);
}

static void test_dl_both_vars(void)
{
	const char *src = "((nil . ((compile-command . \"make all\") "
			  "(buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.compile_command_set == true);
	CHECK(strcmp(s.compile_command, "make all") == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_cmode_skipped(void)
{
	const char *src = "((c-mode . ((indent . 4))) "
			  "(nil . ((buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_nil_not_first(void)
{
	const char *src = "((c-mode . ((x . 1))) "
			  "(nil . ((buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_unknown_var(void)
{
	const char *src = "((nil . ((unknown-var . 5) "
			  "(buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.ignored_entries >= 1);
}

static void test_dl_eval_skipped(void)
{
	const char *src = "((nil . ((eval . (shell-command \"bad\")) "
			  "(buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
	CHECK(s.ignored_entries >= 1);
}

static void test_dl_semicolon_comment(void)
{
	const char *src = "((nil . ((buffer-read-only . t)))) ; a comment";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_leading_quote(void)
{
	const char *src = "'((nil . ((buffer-read-only . t))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_malformed_alist_not_list(void)
{
	const char *src = "((nil . buffer-read-only))";
	struct local_settings s;

	dirlocals_parse(src, strlen(src), &s);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
}

static void test_dl_missing_close_paren(void)
{
	const char *src = "((nil . ((buffer-read-only . t)))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) != 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_UNSET);
}

static void test_dl_nul_inside_sexp(void)
{
	/* A NUL byte inside a parenthesised sexp used to hang the parser:
	 * every atom scan walked "while not a delimiter", and strchr()
	 * calls '\0' a delimiter, so nothing consumed it and the reader
	 * spun on that one byte.  A .dir-locals.el is read from disk and
	 * may contain anything, so this hung the editor.  Found by
	 * test/fuzz_dirlocals once its corpus had seeds. */
	static const char src[]
	    = "((nil . ((tab-width . 4\0x) (buffer-read-only . t))))";
	struct local_settings s;

	/* The point is that this returns at all; that it still finds the
	 * setting after the NUL says the reader kept its place. */
	CHECK(dirlocals_parse(src, sizeof(src) - 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_nul_terminates_value(void)
{
	static const char src[] = "((nil . ((buffer-read-only . t))))\0junk";
	struct local_settings s;

	CHECK(dirlocals_parse(src, sizeof(src) - 1, &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_TRUE);
}

static void test_dl_excessive_nesting(void)
{
	char buf[512];
	int i;
	struct local_settings s;

	if (sizeof(buf) < 130) {
		return;
	}
	i = 0;
	while (i < 95 && i + 1 < (int)sizeof(buf)) {
		buf[i++] = '(';
	}
	while (i < 190 && i + 1 < (int)sizeof(buf)) {
		buf[i++] = ')';
	}
	buf[i] = '\0';

	CHECK(dirlocals_parse(buf, strlen(buf), &s) != 0);
}

static void test_dl_oversized_input(void)
{
	struct local_settings s;
	char *huge;
	size_t sz;

	sz = 66000;
	huge = malloc(sz);
	if (!huge) {
		return;
	}
	memset(huge, 'x', sz);
	huge[0] = '(';
	huge[1] = ')';
	huge[sz - 1] = '\0';

	CHECK(dirlocals_parse(huge, sz - 1, &s) != 0);
	free(huge);
}

static void test_dl_oversized_command_string(void)
{
	struct local_settings s;
	char buf[KG_COMPILE_COMMAND_MAX + 512];
	char payload[KG_COMPILE_COMMAND_MAX + 256];
	int i, n;

	for (i = 0; i < KG_COMPILE_COMMAND_MAX + 200; i++) {
		payload[i] = 'A';
	}
	payload[i] = '\0';

	n = snprintf(buf, sizeof(buf), "((nil . ((compile-command . \"%s\"))))",
	    payload);

	CHECK(dirlocals_parse(buf, (size_t)n, &s) == 0);
	CHECK(s.compile_command_set == false);
	CHECK(s.malformed_entries >= 1);
}

static void test_dl_ro_nil(void)
{
	const char *src = "((nil . ((buffer-read-only . nil))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_FALSE);
}

static void test_dl_duplicate_last_wins_dl(void)
{
	const char *src = "((nil . ((buffer-read-only . t) "
			  "(buffer-read-only . nil))))";
	struct local_settings s;

	CHECK(dirlocals_parse(src, strlen(src), &s) == 0);
	CHECK(s.buffer_read_only == LOCAL_BOOL_FALSE);
}

/* ---- .dir-locals.el directory traversal tests ---- */

static void rmtree_dl(const char *path)
{
	struct dirent *de;
	struct stat st;
	DIR *dp = opendir(path);
	char child[512];

	if (!dp) {
		return;
	}
	while ((de = readdir(dp)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
			continue;
		}
		snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
		if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
			rmtree_dl(child);
		} else {
			unlink(child);
		}
	}
	closedir(dp);
	rmdir(path);
}

static void test_dl_find_root_level(void)
{
	char tmpl[] = "/tmp/kg-dl-XXXXXX";
	char *root = mkdtemp(tmpl);
	char scratch[256];
	char src_path[512];
	char dl_path[1024];
	char found[PATH_MAX];

	CHECK(root != NULL);
	snprintf(scratch, sizeof(scratch), "%s/", root);

	snprintf(dl_path, sizeof(dl_path), "%s/.dir-locals.el", root);
	{
		FILE *fp = fopen(dl_path, "w");

		CHECK(fp != NULL);
		if (!fp) {
			return;
		}
		fprintf(fp, "((nil . ((buffer-read-only . t))))\n");
		fclose(fp);
	}

	snprintf(src_path, sizeof(src_path), "%s/src", root);
	mkdir(src_path, 0700);
	{
		char fpath[1024];

		snprintf(fpath, sizeof(fpath), "%s/file.c", src_path);
		FILE *fp = fopen(fpath, "w");

		if (fp) {
			fclose(fp);
		}
	}

	CHECK(dirlocals_find(src_path, found, sizeof(found)) == 0);
	CHECK(strcmp(found, dl_path) == 0);

	rmtree_dl(scratch);
}

static void test_dl_find_nearest_wins(void)
{
	char tmpl[] = "/tmp/kg-dl-XXXXXX";
	char *root = mkdtemp(tmpl);
	char scratch[256];
	char src_path[512];
	char root_dl[1024];
	char src_dl[1024];
	char found[PATH_MAX];
	char content[1024];
	struct local_settings s;
	FILE *fp;
	long fsz;

	CHECK(root != NULL);
	snprintf(scratch, sizeof(scratch), "%s/", root);

	snprintf(root_dl, sizeof(root_dl), "%s/.dir-locals.el", root);
	fp = fopen(root_dl, "w");
	CHECK(fp != NULL);
	if (!fp) {
		return;
	}
	fprintf(fp, "((nil . ((buffer-read-only . t))))\n");
	fclose(fp);

	snprintf(src_path, sizeof(src_path), "%s/src", root);
	mkdir(src_path, 0700);
	snprintf(src_dl, sizeof(src_dl), "%s/.dir-locals.el", src_path);
	fp = fopen(src_dl, "w");
	CHECK(fp != NULL);
	if (!fp) {
		return;
	}
	fprintf(fp, "((nil . ((buffer-read-only . nil))))\n");
	fclose(fp);

	{
		char fpath[1024];

		snprintf(fpath, sizeof(fpath), "%s/file.c", src_path);
		fp = fopen(fpath, "w");
		if (fp) {
			fclose(fp);
		}
	}

	CHECK(dirlocals_find(src_path, found, sizeof(found)) == 0);
	CHECK(strcmp(found, src_dl) == 0);

	fp = fopen(found, "r");
	CHECK(fp != NULL);
	if (!fp) {
		return;
	}
	fsz = (long)fread(content, 1, sizeof(content) - 1, fp);
	fclose(fp);
	if (fsz > 0) {
		content[fsz] = '\0';
		local_settings_init(&s);
		CHECK(dirlocals_parse(content, (size_t)fsz, &s) == 0);
		CHECK(s.buffer_read_only == LOCAL_BOOL_FALSE);
	}

	rmtree_dl(scratch);
}

static void test_dl_find_nonexistent(void)
{
	char found[PATH_MAX];

	CHECK(dirlocals_find(
		  "/tmp/kg-dl-no-such-file-xyzzy-99999.c", found, sizeof(found))
	    == -1);
	CHECK(dirlocals_find("", found, sizeof(found)) == -1);
	CHECK(dirlocals_find(NULL, found, sizeof(found)) == -1);
}

/* ---- One value applier, three envelopes ----
 *
 * The `-*- ... -*-` line, the `Local Variables:` block and
 * `.dir-locals.el` scan differently on purpose, but a name they all
 * find, and a value they all read, has to mean the same thing in each.
 * That used to be three copies of "lower-case the token, compare it
 * against t and nil, otherwise count it malformed"; this matrix is what
 * keeps the one applier honest. */

static struct local_settings via_modeline(const char *name, const char *value)
{
	char line[512];
	erow rows[1];
	struct local_settings s;

	snprintf(line, sizeof(line), "-*- %s: %s -*-", name, value);
	setup_one_row(&rows[0], line);
	(void)localvars_parse_modeline(rows, 1, &s);
	return s;
}

static struct local_settings via_footer(const char *name, const char *value)
{
	char middle[512];
	const char *lines[3];

	snprintf(middle, sizeof(middle), "# %s: %s", name, value);
	lines[0] = "# Local Variables:";
	lines[1] = middle;
	lines[2] = "# End:";
	return parse_footer_lines(lines, 3);
}

static struct local_settings via_dirlocals(const char *name, const char *value)
{
	char src[512];
	struct local_settings s;

	snprintf(src, sizeof(src), "((nil . ((%s . %s))))", name, value);
	(void)dirlocals_parse(src, strlen(src), &s);
	return s;
}

struct envelope_case {
	const char *name;
	const char *value;
	enum local_bool_value read_only;
	const char *compile_command; /* NULL when it must stay unset */
	unsigned malformed;
	unsigned ignored;
};

static void check_envelope(const char *envelope, const struct envelope_case *c,
    const struct local_settings *s)
{
	CHECKF(s->buffer_read_only == c->read_only,
	    "%s: %s: %s -> read_only %d, expected %d", envelope, c->name,
	    c->value, (int)s->buffer_read_only, (int)c->read_only);
	CHECKF(s->compile_command_set == (c->compile_command != NULL),
	    "%s: %s: %s -> compile_command_set %d", envelope, c->name, c->value,
	    (int)s->compile_command_set);
	if (c->compile_command && s->compile_command_set) {
		CHECKF(strcmp(s->compile_command, c->compile_command) == 0,
		    "%s: %s -> compile_command \"%s\"", envelope, c->name,
		    s->compile_command);
	}
	CHECKF(s->malformed_entries == c->malformed,
	    "%s: %s: %s -> %u malformed, expected %u", envelope, c->name,
	    c->value, s->malformed_entries, c->malformed);
	CHECKF(s->ignored_entries == c->ignored,
	    "%s: %s: %s -> %u ignored, expected %u", envelope, c->name,
	    c->value, s->ignored_entries, c->ignored);
}

static void test_same_value_through_every_envelope(void)
{
	static const struct envelope_case cases[] = {
		{ "buffer-read-only", "t", LOCAL_BOOL_TRUE, NULL, 0, 0 },
		{ "buffer-read-only", "nil", LOCAL_BOOL_FALSE, NULL, 0, 0 },
		/* Emacs reads these symbols case-insensitively. */
		{ "buffer-read-only", "T", LOCAL_BOOL_TRUE, NULL, 0, 0 },
		{ "buffer-read-only", "NIL", LOCAL_BOOL_FALSE, NULL, 0, 0 },
		{ "buffer-read-only", "yes", LOCAL_BOOL_UNSET, NULL, 1, 0 },
		/* Longer than the applier's token buffer: malformed, not
		 * truncated into a match. */
		{ "buffer-read-only", "ttttttttttttttttttt", LOCAL_BOOL_UNSET,
		    NULL, 1, 0 },
		{ "compile-command", "\"make -k\"", LOCAL_BOOL_UNSET, "make -k",
		    0, 0 },
		/* Nothing kg knows: consumed, counted, never applied. */
		{ "no-such-variable", "t", LOCAL_BOOL_UNSET, NULL, 0, 1 },
		{ "no-such-variable", "\"x\"", LOCAL_BOOL_UNSET, NULL, 0, 1 },
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
		struct local_settings s;

		s = via_modeline(cases[i].name, cases[i].value);
		check_envelope("modeline", &cases[i], &s);
		s = via_footer(cases[i].name, cases[i].value);
		check_envelope("footer", &cases[i], &s);
		s = via_dirlocals(cases[i].name, cases[i].value);
		check_envelope("dir-locals", &cases[i], &s);
	}
}

int main(void)
{
	RUN(test_compile_command_only);
	RUN(test_read_only_only);
	RUN(test_both_variables);
	RUN(test_extra_whitespace);
	RUN(test_semicolon_inside_quoted_command);
	RUN(test_escaped_quote_inside_command);
	RUN(test_first_line_comment_prefix);
	RUN(test_shebang_second_line_modeline);
	RUN(test_unterminated_modeline);
	RUN(test_unterminated_string);
	RUN(test_unsupported_var_before_supported);
	RUN(test_duplicate_last_wins);
	RUN(test_buffer_read_only_nil);
	RUN(test_modeline_paren_hides_semicolon);
	RUN(test_modeline_blank_segments_are_not_errors);
	RUN(test_modeline_empty_name_is_malformed);
	RUN(test_modeline_close_marker_needs_three_bytes);
	RUN(test_modeline_overlong_name_is_ignored);
	RUN(test_merge_unset_source_leaves_destination_intact);
	RUN(test_merge_set_compile_command_overwrites_destination);
	RUN(test_merge_set_ro_overwrites_unset_ro_leaves);
	RUN(test_merge_counters_accumulate);
	RUN(test_footer_hash_comment_ro);
	RUN(test_footer_c_block_ro);
	RUN(test_footer_no_prefix_ro);
	RUN(test_footer_case_variants);
	RUN(test_footer_missing_end);
	RUN(test_footer_outside_3000_bytes);
	RUN(test_footer_form_feed_excludes_early_block);
	RUN(test_footer_continued_compile_command);
	RUN(test_footer_override_modeline);
	RUN(test_footer_invalid_ro_value);
	RUN(test_dl_read_only_t);
	RUN(test_dl_compile_command);
	RUN(test_dl_both_vars);
	RUN(test_dl_cmode_skipped);
	RUN(test_dl_nil_not_first);
	RUN(test_dl_unknown_var);
	RUN(test_dl_eval_skipped);
	RUN(test_dl_semicolon_comment);
	RUN(test_dl_leading_quote);
	RUN(test_dl_malformed_alist_not_list);
	RUN(test_dl_missing_close_paren);
	RUN(test_dl_nul_inside_sexp);
	RUN(test_dl_nul_terminates_value);
	RUN(test_dl_excessive_nesting);
	RUN(test_dl_oversized_input);
	RUN(test_dl_oversized_command_string);
	RUN(test_dl_ro_nil);
	RUN(test_dl_duplicate_last_wins_dl);
	RUN(test_dl_find_root_level);
	RUN(test_dl_find_nearest_wins);
	RUN(test_dl_find_nonexistent);
	RUN(test_same_value_through_every_envelope);
	return test_summary();
}
