#include "../src/def.h"
#include "../src/localvars.h"
#include "test.h"
#include <stdio.h>
#include <string.h>

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
	return test_summary();
}
