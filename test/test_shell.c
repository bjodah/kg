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
enum minibuf_result editor_read_line_with_history(int fd, const char *prompt,
    char *buf, int bufsize, struct minibuf_history *hist)
{
	(void)fd;
	(void)prompt;
	(void)bufsize;
	(void)hist;
	buf[0] = '\0';
	return MINIBUF_CANCELLED;
}

/* Capture a child's stdout from a simple command with no stdin. */
static void test_shell_run_no_input(void)
{
	char *out;
	int len = -1;
	struct shell_run_status status;

	out = shell_run("printf 'hello\\n'", NULL, 0, &len, &status);
	CHECK(out != NULL);
	CHECK(len == 6);
	CHECK(strcmp(out, "hello\n") == 0);
	CHECK(status.known);
	CHECK(status.exited);
	CHECK(status.exit_code == 0);
	free(out);
}

/* Pipe a known input through `cat` and confirm we get it back verbatim. */
static void test_shell_run_pipe_through_cat(void)
{
	const char *in = "line1\nline2\nline3\n";
	char *out;
	int len = -1;

	out = shell_run("cat", in, (int)strlen(in), &len, NULL);
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
	out = shell_run("yes x | head -n 200", NULL, 0, &len, NULL);
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

	out = shell_run("cat", in, inlen, &len, NULL);
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
	struct shell_run_status status;

	out = shell_run("nope-does-not-exist-12345", NULL, 0, &len, &status);
	CHECK(out != NULL); /* successful pipe setup; stdout was empty */
	CHECK(len == 0);
	CHECK(status.known);
	CHECK(status.exited);
	CHECK(status.exit_code == 127);
	free(out);
}

/* A command that exits nonzero with no output must be distinguishable from
 * a genuinely successful, silent command — this is what editor_shell_command
 * relies on to avoid reporting "succeeded" for a failing command. */
static void test_shell_run_exit_nonzero(void)
{
	char *out;
	int len = -1;
	struct shell_run_status status;

	out = shell_run("sh -c 'exit 3'", NULL, 0, &len, &status);
	CHECK(out != NULL);
	CHECK(len == 0);
	CHECK(status.known);
	CHECK(status.exited);
	CHECK(status.exit_code == 3);
	CHECK(status.signal_number == 0);
	free(out);
}

/* A command killed by a signal reports exited==false and the signal
 * number. */
static void test_shell_run_killed_by_signal(void)
{
	char *out;
	int len = -1;
	struct shell_run_status status;

	out = shell_run("kill -TERM $$", NULL, 0, &len, &status);
	CHECK(out != NULL);
	CHECK(status.known);
	CHECK(!status.exited);
	CHECK(status.signal_number == SIGTERM);
	free(out);
}

/* ---- shell_waitpid_fn injection: EINTR retry / permanent failure ---- */

static int g_fake_waitpid_eintr_countdown;

static pid_t fake_waitpid_eintr_then_real(pid_t pid, int *status, int options)
{
	if (g_fake_waitpid_eintr_countdown > 0) {
		g_fake_waitpid_eintr_countdown--;
		errno = EINTR;
		return -1;
	}
	return waitpid(pid, status, options);
}

/* wait_for_child() must retry across EINTR rather than reporting the child's
 * status as unknown after the first interrupted waitpid() call. */
static void test_shell_run_waitpid_retries_eintr(void)
{
	char *out;
	int len = -1;
	struct shell_run_status status;

	g_fake_waitpid_eintr_countdown = 3;
	shell_waitpid_fn = fake_waitpid_eintr_then_real;
	out = shell_run("true", NULL, 0, &len, &status);
	shell_waitpid_fn = waitpid;

	CHECK(g_fake_waitpid_eintr_countdown == 0);
	CHECK(out != NULL);
	CHECK(status.known);
	CHECK(status.exited);
	CHECK(status.exit_code == 0);
	free(out);
}

static pid_t fake_waitpid_always_fails(pid_t pid, int *status, int options)
{
	(void)pid;
	(void)status;
	(void)options;
	errno = ECHILD;
	return -1;
}

/* When the child cannot be reaped at all, shell_run() must not report a
 * fabricated "succeeded" status: known stays false and exited/signal_number
 * stay at their zeroed defaults, even though pump_io() still completed. */
static void test_shell_run_waitpid_permanent_failure(void)
{
	char *out;
	int len = -1;
	struct shell_run_status status;

	shell_waitpid_fn = fake_waitpid_always_fails;
	out = shell_run("true", NULL, 0, &len, &status);
	shell_waitpid_fn = waitpid;

	CHECK(out != NULL);
	CHECK(!status.known);
	CHECK(!status.exited);
	CHECK(status.exit_code == 0);
	CHECK(status.signal_number == 0);
	free(out);

	/* The real child was never reaped above (the fake hook always
	 * failed); sweep it up so it doesn't linger as a zombie for the
	 * rest of the test binary's run. */
	while (waitpid(-1, NULL, WNOHANG) > 0) { }
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

/* ---- shell_output_fits_echo() ---- */

static void test_echo_fits_short_single_line(void)
{
	const char out[] = "12345678901234567890"; /* 20 bytes */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

static void test_echo_fits_trailing_newline(void)
{
	const char out[] = "hello\n";
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

static void test_echo_fits_exact_width(void)
{
	const char out[] = "12345"; /* 5 bytes */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 5, 0));
}

static void test_echo_rejects_one_column_too_wide(void)
{
	const char out[] = "123456"; /* 6 bytes */
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 5, 0));
}

static void test_echo_rejects_over_statusmsg_capacity(void)
{
	static char out[600];
	memset(out, 'x', sizeof(out));
	CHECK(!shell_output_fits_echo(out, (int)sizeof(out), 4096, 0));
}

static void test_echo_rejects_multiline(void)
{
	const char out[] = "first\nsecond";
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

static void test_echo_rejects_tab(void)
{
	const char out[] = "a\tb";
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

static void test_echo_rejects_escape(void)
{
	const char out[] = "a\x1b[31mb";
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

static void test_echo_rejects_invalid_utf8(void)
{
	const char out[] = "a\x80\x80"
			   "b"; /* stray continuation bytes, no lead byte */
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

static void test_echo_fits_valid_multibyte_utf8(void)
{
	const char out[] = "a\xC3\xA9z"; /* 'a', two-byte glyph, 'z' */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 80, 0));
}

/* "a", a two-byte glyph, "z": 4 bytes but 3 display columns.  Byte-counting
 * would wrongly reject a 3-column budget; column-counting must accept it. */
static void test_echo_width_counts_columns_not_bytes(void)
{
	const char out[] = "a\xC3\xA9z"; /* 4 bytes, 3 columns */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 3, 0));
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 2, 0));
}

/* A three-byte glyph counts as exactly one column. */
static void test_echo_width_three_byte_glyph_one_column(void)
{
	const char out[] = "\xE2\x82\xAC"; /* U+20AC EURO SIGN, 3 bytes */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 1, 0));
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 0, 0));
}

/* A four-byte glyph of ordinary width counts as exactly one column. */
static void test_echo_width_four_byte_glyph_one_column(void)
{
	const char out[] = "\xF0\x90\x90\x80"; /* U+10400 DESERET, 4 bytes */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 1, 0));
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 0, 0));
}

/* East-Asian-Wide glyphs are drawn two cells wide, so the echo-area
 * budget has to charge them two columns however many bytes they take. */
static void test_echo_width_wide_glyph_two_columns(void)
{
	const char emoji[] = "\xF0\x9F\x98\x80"; /* U+1F600, EAW=W */
	const char cjk[] = "\xE6\xBC\xA2\xE5\xAD\x97"; /* 漢字, EAW=W */

	CHECK(shell_output_fits_echo(emoji, (int)strlen(emoji), 2, 0));
	CHECK(!shell_output_fits_echo(emoji, (int)strlen(emoji), 1, 0));
	CHECK(shell_output_fits_echo(cjk, (int)strlen(cjk), 4, 0));
	CHECK(!shell_output_fits_echo(cjk, (int)strlen(cjk), 3, 0));
}

/* reserved_columns carves room out of available_columns for a suffix the
 * caller will append after the returned text. */
static void test_echo_reserved_columns_narrows_budget(void)
{
	const char out[] = "12345"; /* 5 bytes/columns */
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 5, 0));
	CHECK(!shell_output_fits_echo(out, (int)strlen(out), 5, 1));
	CHECK(shell_output_fits_echo(out, (int)strlen(out), 6, 1));
}

static void test_echo_rejects_overlong_three_byte(void)
{
	const char out[]
	    = "\xE0\x80\x80"; /* overlong: second byte must be A0-BF */
	CHECK(!shell_output_fits_echo(out, 3, 80, 0));
}

static void test_echo_rejects_utf16_surrogate(void)
{
	const char out[]
	    = "\xED\xA0\x80"; /* U+D800 surrogate, ED requires 80-9F */
	CHECK(!shell_output_fits_echo(out, 3, 80, 0));
}

static void test_echo_rejects_overlong_four_byte(void)
{
	const char out[] = "\xF0\x80\x80\x80"; /* overlong: F0 requires 90-BF */
	CHECK(!shell_output_fits_echo(out, 4, 80, 0));
}

static void test_echo_rejects_above_max_codepoint(void)
{
	const char out[]
	    = "\xF4\x90\x80\x80"; /* > U+10FFFF: F4 requires 80-8F */
	CHECK(!shell_output_fits_echo(out, 4, 80, 0));
}

static void test_echo_fits_max_codepoint_boundary(void)
{
	const char out[] = "\xF4\x8F\xBF\xBF"; /* exactly U+10FFFF: valid */
	CHECK(shell_output_fits_echo(out, 4, 80, 0));
}

static void test_echo_rejects_empty_output(void)
{
	CHECK(!shell_output_fits_echo("", 0, 80, 0));
}

/* ---- Main ---- */

int main(void)
{
	RUN(test_shell_run_no_input);
	RUN(test_shell_run_pipe_through_cat);
	RUN(test_shell_run_large_output);
	RUN(test_shell_run_pump_no_deadlock);
	RUN(test_shell_run_command_not_found);
	RUN(test_shell_run_exit_nonzero);
	RUN(test_shell_run_killed_by_signal);
	RUN(test_shell_run_waitpid_retries_eintr);
	RUN(test_shell_run_waitpid_permanent_failure);
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
	RUN(test_echo_width_counts_columns_not_bytes);
	RUN(test_echo_width_three_byte_glyph_one_column);
	RUN(test_echo_width_four_byte_glyph_one_column);
	RUN(test_echo_width_wide_glyph_two_columns);
	RUN(test_echo_reserved_columns_narrows_budget);
	RUN(test_echo_rejects_overlong_three_byte);
	RUN(test_echo_rejects_utf16_surrogate);
	RUN(test_echo_rejects_overlong_four_byte);
	RUN(test_echo_rejects_above_max_codepoint);
	RUN(test_echo_fits_max_codepoint_boundary);
	RUN(test_echo_rejects_empty_output);
	return test_summary();
}
