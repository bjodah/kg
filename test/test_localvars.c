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
	return test_summary();
}
