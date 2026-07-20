#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../src/compile.h"
#include "../src/def.h"
#include "../src/localvars.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return -1;
}

void editor_refresh_screen(void) { }
int editor_read_key(int fd)
{
	(void)fd;
	return 0;
}

void buf_save_current_state(void) { }

void editor_free_row(erow *row) { (void)row; }

int shell_run_capture(
    const char *c, const char *d, size_t m, struct shell_capture_result *r)
{
	(void)c;
	(void)d;
	(void)m;
	memset(r, 0, sizeof(*r));
	r->output = NULL;
	return -1;
}

struct editor_syntax compilation_syntax
    = { "Compilation", NULL, NULL, "", "", "", 0 };

int buf_replace_special_text(const char *name, struct editor_syntax *syntax,
    const char *text, size_t text_length, int readonly)
{
	(void)name;
	(void)syntax;
	(void)text;
	(void)text_length;
	(void)readonly;
	return -1;
}

void win_display_buffer_other_window(int buffer_index) { (void)buffer_index; }
void win_position_at_end(int buffer_index) { (void)buffer_index; }
void buf_restore_from_slot(int idx) { (void)idx; }

int buf_prepare_special_text(const char *name, struct editor_syntax *syntax,
    int readonly)
{
	(void)name;
	(void)syntax;
	(void)readonly;
	return 0;
}

int buf_append_special_text(int buffer_index, const char *text,
    size_t text_length)
{
	(void)buffer_index;
	(void)text;
	(void)text_length;
	return 0;
}

void buf_clear_special_text(int buffer_index)
{
	(void)buffer_index;
}

void buf_truncate_last_row(int buffer_index, size_t len_to_remove)
{
	(void)buffer_index;
	(void)len_to_remove;
}

static void test_transcript_command_and_directory(void)
{
	struct shell_capture_result cap;
	char *t;

	memset(&cap, 0, sizeof(cap));
	cap.output = strdup("out\n");
	if (!cap.output) {
		return;
	}
	cap.output_length = strlen(cap.output);
	cap.exited = true;
	cap.exit_code = 0;

	t = compilation_format_transcript("make all", "/tmp/proj", 0, &cap);
	CHECK(t != NULL);
	CHECK(strstr(t, "Compilation started in /tmp/proj") != NULL);
	CHECK(strstr(t, "$ make all") != NULL);
	free(cap.output);
	free(t);
}

static void test_transcript_exit_code(void)
{
	struct shell_capture_result cap;
	char *t;

	memset(&cap, 0, sizeof(cap));
	cap.output = strdup("err\n");
	if (!cap.output) {
		return;
	}
	cap.output_length = strlen(cap.output);
	cap.exited = true;
	cap.exit_code = 7;

	t = compilation_format_transcript("make", "/tmp", 0, &cap);
	CHECK(t != NULL);
	CHECK(strstr(t, "Compilation finished with exit code 7") != NULL);
	free(cap.output);
	free(t);
}

static void test_transcript_signal(void)
{
	struct shell_capture_result cap;
	char *t;

	memset(&cap, 0, sizeof(cap));
	cap.output = strdup("killed\n");
	if (!cap.output) {
		return;
	}
	cap.output_length = strlen(cap.output);
	cap.exited = false;
	cap.signal_number = 15;

	t = compilation_format_transcript("make", "/tmp", 0, &cap);
	CHECK(t != NULL);
	CHECK(strstr(t, "Compilation terminated by signal 15") != NULL);
	free(cap.output);
	free(t);
}

static void test_transcript_truncation(void)
{
	struct shell_capture_result cap;
	char *t;

	memset(&cap, 0, sizeof(cap));
	cap.output = strdup("x");
	if (!cap.output) {
		return;
	}
	cap.output_length = 1;
	cap.exited = true;
	cap.exit_code = 0;
	cap.truncated = true;

	t = compilation_format_transcript("cmd", "/tmp", 1024, &cap);
	CHECK(t != NULL);
	CHECK(strstr(t, "[kg: compilation output truncated after 1024 bytes]")
	    != NULL);
	free(cap.output);
	free(t);
}

static void test_transcript_output_no_newline(void)
{
	struct shell_capture_result cap;
	char *t;

	memset(&cap, 0, sizeof(cap));
	cap.output = strdup("noeol");
	if (!cap.output) {
		return;
	}
	cap.output_length = strlen(cap.output);
	cap.exited = true;
	cap.exit_code = 0;

	t = compilation_format_transcript("cmd", "/tmp", 0, &cap);
	CHECK(t != NULL);
	CHECK(strstr(t, "noeol\n\nCompilation finished with exit code 0")
	    != NULL);
	free(cap.output);
	free(t);
}

static void test_resolve_null_filename(void)
{
	char dir[PATH_MAX];
	char cwd[PATH_MAX];

	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	CHECK(compilation_resolve_directory(NULL, dir, sizeof(dir)) == 0);
	CHECK(strcmp(dir, cwd) == 0);
}

static void test_resolve_special_buffer(void)
{
	char dir[PATH_MAX];
	char cwd[PATH_MAX];

	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	CHECK(
	    compilation_resolve_directory("*scratch*", dir, sizeof(dir)) == 0);
	CHECK(strcmp(dir, cwd) == 0);
}

static void test_resolve_relative_no_slash(void)
{
	char dir[PATH_MAX];
	char cwd[PATH_MAX];

	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	CHECK(compilation_resolve_directory("foo.c", dir, sizeof(dir)) == 0);
	CHECK(strcmp(dir, cwd) == 0);
}

static void test_resolve_absolute(void)
{
	char tmpl[] = "/tmp/kg-compile-resolve-XXXXXX";
	char *tmpdir;
	char dir[PATH_MAX];
	char fname[512];

	tmpdir = mkdtemp(tmpl);
	CHECK(tmpdir != NULL);
	snprintf(fname, sizeof(fname), "%s/bar.c", tmpdir);
	{
		FILE *fp = fopen(fname, "w");
		CHECK(fp != NULL);
		if (!fp) {
			return;
		}
		fclose(fp);
	}
	CHECK(compilation_resolve_directory(fname, dir, sizeof(dir)) == 0);
	CHECK(strstr(dir, tmpdir) != NULL || strcmp(dir, tmpdir) == 0);
	{
		const char *base = strrchr(dir, '/');
		CHECK(base != NULL);
		/* The resolved dir must contain the parent component name,
		 * which is the mkdtemp suffix, and must end with it (or be
		 * a canonical form containing it). */
		CHECK(strstr(dir, tmpl + 5) != NULL);
	}
	unlink(fname);
	rmdir(tmpdir);
}

int main(void)
{
	RUN(test_transcript_command_and_directory);
	RUN(test_transcript_exit_code);
	RUN(test_transcript_signal);
	RUN(test_transcript_truncation);
	RUN(test_transcript_output_no_newline);
	RUN(test_resolve_null_filename);
	RUN(test_resolve_special_buffer);
	RUN(test_resolve_relative_no_slash);
	RUN(test_resolve_absolute);
	return test_summary();
}
