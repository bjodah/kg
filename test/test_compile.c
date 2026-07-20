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

static char g_next_command[256] = "";

int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	strncpy(buf, g_next_command, bufsize);
	buf[bufsize - 1] = '\0';
	return 0;
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
    = { "Compilation", NULL, NULL, "", "", "", 0, NULL };

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

/* A minimal flat model of the compilation buffer so streaming tests can
 * observe exactly what the byte-processing state machine appended. Rows are
 * newline-separated in g_model; buf_truncate_last_row removes bytes from the
 * end of the final row only, matching the real helper's no-op-when-too-long
 * behavior. */
static char g_model[1 << 16];
static size_t g_model_len;

int buf_prepare_special_text(
    const char *name, struct editor_syntax *syntax, int readonly)
{
	(void)name;
	(void)syntax;
	(void)readonly;
	g_model_len = 0;
	g_model[0] = '\0';
	return 0;
}

int buf_append_special_text(
    int buffer_index, const char *text, size_t text_length)
{
	(void)buffer_index;
	if (g_model_len + text_length < sizeof(g_model)) {
		memcpy(g_model + g_model_len, text, text_length);
		g_model_len += text_length;
		g_model[g_model_len] = '\0';
	}
	return 0;
}

void buf_clear_special_text(int buffer_index)
{
	(void)buffer_index;
	g_model_len = 0;
	g_model[0] = '\0';
}

void buf_truncate_last_row(int buffer_index, size_t len_to_remove)
{
	(void)buffer_index;
	if (len_to_remove == 0) {
		return;
	}
	size_t last_row_len = 0;
	while (last_row_len < g_model_len
	    && g_model[g_model_len - 1 - last_row_len] != '\n') {
		last_row_len++;
	}
	if (len_to_remove <= last_row_len) {
		g_model_len -= len_to_remove;
		g_model[g_model_len] = '\0';
	}
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

/* Regression for the streaming path: a final line with no trailing newline
 * must be committed exactly once (it was previously mirrored and then flushed
 * again, duplicating it). The output token is produced via octal escapes so
 * it cannot appear in the echoed command header, isolating the output. */
static void test_streaming_no_final_newline(void)
{
	strcpy(g_next_command, "printf '\\132\\132\\132'");
	editor_compile(0);

	for (int i = 0; i < 2000 && compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}

	CHECK(!compilation_is_running());

	const char *first = strstr(g_model, "ZZZ");
	CHECK(first != NULL);
	if (first) {
		CHECK(strstr(first + 1, "ZZZ") == NULL);
	}
	CHECK(strstr(g_model, "Compilation finished with exit code 0") != NULL);
}

int main(void)
{
	RUN(test_streaming_no_final_newline);
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
