#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* test_shell.c — regression tests for the shell-command runner.
 *
 * These exercise shell_run() directly: fork/exec/pipe plumbing and the
 * concurrent non-blocking I/O pump.  The interactive editor wrappers
 * (editor_shell_command / editor_shell_command_on_region) need a PTY
 * and are exercised by hand. */

#include "../src/def.h"
#include "test.h"
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The test exercises shell_run() only; the editor wrappers it calls below
 * are linked in via the full object set, so we stub the one symbol the
 * shared no-yank stubs file does not provide. */
int editor_read_line(int fd, const char *prompt, char *buf, int bufsize)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	buf[0] = '\0';
	return -1;
}

/* Capture a child's stdout from a simple command with no stdin. */
static void test_shell_run_no_input(void)
{
	char *out;
	int len = -1;

	out = shell_run("printf 'hello\\n'", NULL, 0, &len);
	CHECK(out != NULL);
	CHECK(len == 6);
	CHECK(strcmp(out, "hello\n") == 0);
	free(out);
}

/* Pipe a known input through `cat` and confirm we get it back verbatim. */
static void test_shell_run_pipe_through_cat(void)
{
	const char *in = "line1\nline2\nline3\n";
	char *out;
	int len = -1;

	out = shell_run("cat", in, (int)strlen(in), &len);
	CHECK(out != NULL);
	CHECK(len == (int)strlen(in));
	CHECK(memcmp(out, in, len) == 0);
	free(out);
}

/* Verify the pump survives a larger payload than the initial buffer. */
static void test_shell_run_large_output(void)
{
	char *out;
	int len = -1;

	/* 200 lines of "x" → 400 bytes; well above the initial 4 KiB buffer
	 * we still want to make sure realloc growth keeps the buffer NUL-
	 * terminated and the count accurate. */
	out = shell_run("yes x | head -n 200", NULL, 0, &len);
	CHECK(out != NULL);
	CHECK(len == 400);
	CHECK(out[len] == '\0');
	free(out);
}

/* When stdin is large enough to fill the pipe buffer while the child also
 * writes a lot, the pump must avoid the classic write-blocks-while-read-blocks
 * deadlock.  64 KiB through `cat` exercises that. */
static void test_shell_run_pump_no_deadlock(void)
{
	char *in;
	char *out;
	int inlen = 65536;
	int len = -1;
	int i;

	in = malloc(inlen);
	if (!in) {
		CHECK(0);
		return;
	}
	for (i = 0; i < inlen; i++) {
		in[i] = 'A' + (i % 26);
	}

	out = shell_run("cat", in, inlen, &len);
	CHECK(out != NULL);
	CHECK(len == inlen);
	CHECK(memcmp(out, in, inlen) == 0);
	free(out);
	free(in);
}

/* Bogus command: /bin/sh -c returns 127, but shell_run still completes;
 * we should get back a (possibly empty) malloc'd buffer, not NULL or a crash.
 */
static void test_shell_run_command_not_found(void)
{
	char *out;
	int len = -1;

	out = shell_run("nope-does-not-exist-12345", NULL, 0, &len);
	CHECK(out != NULL); /* successful pipe setup; stdout was empty */
	CHECK(len == 0);
	free(out);
}

static void rmtree(const char *path)
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
			rmtree(child);
		} else {
			unlink(child);
		}
	}
	closedir(dp);
	rmdir(path);
}

static void test_capture_stdout(void)
{
	struct shell_capture_result r;

	shell_run_capture("printf 'hello\\n'", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 0);
	CHECK(r.output != NULL);
	CHECK(r.output_length == 6);
	CHECK(strcmp(r.output, "hello\n") == 0);
	CHECK(!r.truncated);
	free(r.output);
}

static void test_capture_stderr(void)
{
	struct shell_capture_result r;

	shell_run_capture("printf 'err\\n' 1>&2", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 0);
	CHECK(r.output != NULL);
	CHECK(strstr(r.output, "err\n") != NULL);
	free(r.output);
}

static void test_capture_both(void)
{
	struct shell_capture_result r;

	shell_run_capture("printf 'out\\n'; printf 'err\\n' 1>&2", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.output != NULL);
	CHECK(strstr(r.output, "out\n") != NULL);
	CHECK(strstr(r.output, "err\n") != NULL);
	free(r.output);
}

static void test_capture_exit_zero(void)
{
	struct shell_capture_result r;

	shell_run_capture("true", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 0);
	CHECK(r.output != NULL);
	CHECK(r.output[0] == '\0');
	CHECK(r.output_length == 0);
	free(r.output);
}

static void test_capture_exit_nonzero(void)
{
	struct shell_capture_result r;

	shell_run_capture("sh -c 'exit 7'", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 7);
	free(r.output);
}

static void test_capture_signal(void)
{
	struct shell_capture_result r;

	shell_run_capture("kill -TERM $$", NULL, 0, &r);
	CHECK(!r.exited);
	CHECK(r.signal_number == SIGTERM);
	free(r.output);
}

static void test_capture_working_dir(void)
{
	struct shell_capture_result r;
	char tmpl[] = "/tmp/kg-shell-capture-XXXXXX";
	const char *dir = mkdtemp(tmpl);

	CHECK(dir != NULL);
	shell_run_capture("pwd", dir, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 0);
	CHECK(r.output != NULL);
	CHECK(strstr(r.output, dir) != NULL);
	free(r.output);
	rmtree(dir);
}

static void test_capture_command_not_found(void)
{
	struct shell_capture_result r;

	shell_run_capture("kg-definitely-not-a-real-cmd-xyz", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 127);
	free(r.output);
}

static void test_capture_truncate(void)
{
	struct shell_capture_result r;

	shell_run_capture("seq 1 100000", NULL, 1024, &r);
	CHECK(r.exited);
	CHECK(r.truncated);
	CHECK(r.output_length <= 1024);
	CHECK(r.output != NULL);
	free(r.output);
}

static void test_capture_empty_output(void)
{
	struct shell_capture_result r;

	shell_run_capture("true", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 0);
	CHECK(r.output != NULL);
	CHECK(r.output_length == 0);
	CHECK(r.output[0] == '\0');
	free(r.output);
}

static void test_capture_closed_stdin(void)
{
	struct shell_capture_result r;

	shell_run_capture("cat", NULL, 0, &r);
	CHECK(r.exited);
	CHECK(r.exit_code == 0);
	CHECK(r.output != NULL);
	CHECK(r.output_length == 0);
	CHECK(r.output[0] == '\0');
	free(r.output);
}

/* ---- shell_output_fits_echo() ---- */

static void test_echo_fits_short_single_line(void)
{
	const char out[] = "12345678901234567890"; /* 20 bytes */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_fits_trailing_newline(void)
{
	const char out[] = "hello\n";
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_fits_exact_width(void)
{
	const char out[] = "12345"; /* 5 bytes */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 5));
}

static void test_echo_rejects_one_column_too_wide(void)
{
	const char out[] = "123456"; /* 6 bytes */
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 5));
}

static void test_echo_rejects_over_statusmsg_capacity(void)
{
	static char out[600];
	memset(out, 'x', sizeof(out));
	CHECK(!shell_output_fits_echo(out, (int)sizeof(out), 4096));
}

static void test_echo_rejects_multiline(void)
{
	const char out[] = "first\nsecond";
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_rejects_tab(void)
{
	const char out[] = "a\tb";
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_rejects_escape(void)
{
	const char out[] = "a\x1b[31mb";
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_rejects_invalid_utf8(void)
{
	const char out[] = "a\x80\x80"
			   "b"; /* stray continuation bytes, no lead byte */
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_fits_valid_multibyte_utf8(void)
{
	const char out[] = "a\xC3\xA9z"; /* 'a', two-byte glyph, 'z' */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 80));
}

static void test_echo_rejects_empty_output(void)
{
	CHECK(!shell_output_fits_echo("", 0, 80));
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_shell_run_no_input);
	RUN(test_shell_run_pipe_through_cat);
	RUN(test_shell_run_large_output);
	RUN(test_shell_run_pump_no_deadlock);
	RUN(test_shell_run_command_not_found);
	RUN(test_capture_stdout);
	RUN(test_capture_stderr);
	RUN(test_capture_both);
	RUN(test_capture_exit_zero);
	RUN(test_capture_exit_nonzero);
	RUN(test_capture_signal);
	RUN(test_capture_working_dir);
	RUN(test_capture_command_not_found);
	RUN(test_capture_truncate);
	RUN(test_capture_empty_output);
	RUN(test_capture_closed_stdin);
	RUN(test_echo_fits_short_single_line);
	RUN(test_echo_fits_trailing_newline);
	RUN(test_echo_fits_exact_width);
	RUN(test_echo_rejects_one_column_too_wide);
	RUN(test_echo_rejects_over_statusmsg_capacity);
	RUN(test_echo_rejects_multiline);
	RUN(test_echo_rejects_tab);
	RUN(test_echo_rejects_escape);
	RUN(test_echo_rejects_invalid_utf8);
	RUN(test_echo_fits_valid_multibyte_utf8);
	RUN(test_echo_rejects_empty_output);
	return test_summary();
}
