#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "../src/compile.h"
#include "../src/def.h"
#include "../src/localvars.h"
#include "../src/syntax.h"
#include "test.h"
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_next_command[256] = "";

/* editor_compile() prompts with history; the ring itself is exercised in
 * test_minibuf, so this only has to hand back the queued command. */
enum minibuf_result editor_read_line_with_history(int fd, const char *prompt,
    char *buf, int bufsize, struct minibuf_history *hist)
{
	(void)fd;
	(void)prompt;
	(void)hist;
	strncpy(buf, g_next_command, bufsize);
	buf[bufsize - 1] = '\0';
	return MINIBUF_ACCEPTED;
}

void editor_refresh_screen(void) { }
struct key_event editor_read_key(int fd)
{
	(void)fd;
	return (struct key_event) { 0, 0 };
}
/* Same answer the editor_read_key() above used to give the hand-rolled
 * prompt this replaced: not a yes. */
int editor_confirm_yn(int fd, const char *fmt, ...)
{
	(void)fd;
	(void)fmt;
	return 0;
}

void editor_free_row(erow *row) { (void)row; }

struct editor_syntax compilation_syntax
    = { "Compilation", NULL, NULL, "", "", "", 0, NULL };

void win_display_buffer_other_window(int buffer_index) { (void)buffer_index; }
void win_position_at_end(int buffer_index) { (void)buffer_index; }
int buf_select(int slot)
{
	(void)slot;
	return 1;
}

/* Buffer identity belongs to bufmgr.c, which this binary does not link: here
 * a handle is its slot and always resolves, so the streaming tests exercise
 * the parser rather than the slot table. */
struct kg_buffer_handle buf_handle(int slot)
{
	struct kg_buffer_handle handle = { slot, 1, 0 };

	return handle;
}
int buf_handle_slot(struct kg_buffer_handle handle) { return handle.slot; }

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

/* The streaming seams: a stack-allocated state, no child, no globals.  A
 * committed line is charged and mirrored, the still-open one is only
 * mirrored. */
static void test_stream_seam_drives_parser(void)
{
	struct compilation_state s;

	memset(&s, 0, sizeof(s));
	compilation_stream_reset(&s, 64);
	buf_prepare_special_text("*compilation*", &compilation_syntax, 1);

	compilation_process_bytes(&s, "one\ntw", 6);
	compilation_process_bytes(&s, "o", 1);
	CHECK(strcmp(g_model, "one\ntwo") == 0);
	/* Three body bytes, the newline that terminated them, and the three
	 * bytes still pending: every retained byte is charged. */
	CHECK(s.stored_output == 7);
	CHECK(s.pending_line_length == 3);
	CHECK(!s.truncated);

	compilation_stream_reset(&s, 64);
	CHECK(s.pending_line == NULL);
	CHECK(s.stored_output == 0);
}

/* Feed `text` to `s` in `chunk`-byte pieces, the way short pipe reads do. */
static void feed_chunks(
    struct compilation_state *s, const char *text, size_t len, size_t chunk)
{
	for (size_t i = 0; i < len; i += chunk) {
		size_t n = len - i < chunk ? len - i : chunk;
		compilation_process_bytes(s, text + i, n);
	}
}

/* A stream of nothing but newlines used to append rows forever: the body of
 * each line was empty, so stored_output never moved.  Retained newlines are
 * charged now, so the flood stops at the cap. */
static void test_budget_charges_newlines(void)
{
	struct compilation_state s;
	char flood[10000];

	memset(&s, 0, sizeof(s));
	compilation_stream_reset(&s, 16);
	buf_prepare_special_text("*compilation*", &compilation_syntax, 1);

	memset(flood, '\n', sizeof(flood));
	feed_chunks(&s, flood, sizeof(flood), 4096);

	CHECK(g_model_len == 16);
	CHECK(s.stored_output == 16);
	CHECK(s.truncated);
	compilation_stream_reset(&s, 0);
}

/* One unterminated line must not grow the pending buffer past what the budget
 * could still fund, however long the child keeps writing. */
static void test_budget_bounds_pending_line(void)
{
	struct compilation_state s;
	char *line = malloc(1 << 20);

	CHECK(line != NULL);
	if (!line) {
		return;
	}
	memset(line, 'a', 1 << 20);

	memset(&s, 0, sizeof(s));
	compilation_stream_reset(&s, 64);
	buf_prepare_special_text("*compilation*", &compilation_syntax, 1);

	feed_chunks(&s, line, 1 << 20, 4096);

	CHECK(s.pending_line_cap <= 65);
	CHECK(s.pending_line_length == 64);
	CHECK(s.stored_output == 64);
	CHECK(g_model_len == 64);
	CHECK(s.truncated);
	compilation_stream_reset(&s, 0);
	free(line);
}

/* The cap counts every retained byte: three body bytes plus the newline is
 * exactly four.  One below fits with room, one above truncates. */
static void test_budget_boundary(void)
{
	struct compilation_state s;

	memset(&s, 0, sizeof(s));
	compilation_stream_reset(&s, 4);
	buf_prepare_special_text("*compilation*", &compilation_syntax, 1);
	compilation_process_bytes(&s, "abc\n", 4);
	CHECK(g_model_len == 4);
	CHECK(s.stored_output == 4);
	CHECK(!s.truncated);

	compilation_process_bytes(&s, "d", 1);
	CHECK(g_model_len == 4);
	CHECK(s.truncated);

	compilation_stream_reset(&s, 5);
	buf_prepare_special_text("*compilation*", &compilation_syntax, 1);
	compilation_process_bytes(&s, "abc\n", 4);
	CHECK(!s.truncated);
	compilation_process_bytes(&s, "d", 1);
	CHECK(g_model_len == 5);
	CHECK(!s.truncated);
	compilation_stream_reset(&s, 0);
}

/* Escape sequences and CRLF pairs split across a read boundary must be
 * recognised exactly as if they had arrived together. */
static void test_stream_split_sequences(void)
{
	struct compilation_state s;

	memset(&s, 0, sizeof(s));
	compilation_stream_reset(&s, 1024);
	buf_prepare_special_text("*compilation*", &compilation_syntax, 1);

	/* CSI split between ESC and '[', and again inside its parameters. */
	feed_chunks(&s, "a\x1b[1;32mb\n", 10, 1);
	/* OSC split across its BEL terminator. */
	feed_chunks(&s, "\x1b]0;title\ac\n", 12, 3);
	/* CRLF split across the pair. */
	feed_chunks(&s, "d\r\ne\n", 5, 2);

	CHECK(strcmp(g_model, "ab\nc\nd\ne\n") == 0);
	CHECK(s.stored_output == 9);
	compilation_stream_reset(&s, 0);
}

/* The budget setter reaches a real run, and hitting the cap stops nothing
 * else: the pipe is still drained, the child still reaped, the exit status
 * still reported, and the marker written exactly once. */
static void test_truncated_run_still_finishes(void)
{
	const char *marker = "[kg: compilation output truncated after 2 bytes]";
	const char *found;

	compilation_set_maximum_output(2);
	strcpy(
	    g_next_command, "printf '\\132\\132\\132\\132\\n%.0s' $(seq 200)");
	editor_compile(0);

	for (int i = 0; i < 5000 && compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}
	compilation_set_maximum_output(8 * 1024 * 1024);

	CHECK(!compilation_is_running());
	CHECK(strstr(g_model, "ZZZZ") == NULL);
	found = strstr(g_model, marker);
	CHECK(found != NULL);
	if (found) {
		CHECK(strstr(found + 1, marker) == NULL);
	}
	CHECK(strstr(g_model, "Compilation finished with exit code 0") != NULL);
}

/* The pid the child announced with "PID=", or 0 if it has not yet.  The
 * command line is echoed into the buffer too, so the first "PID=" in the
 * model is the header's literal one, with a "$" where a digit would be. */
static pid_t model_announced_pid(void)
{
	const char *at = g_model;

	while ((at = strstr(at, "PID=")) != NULL) {
		at += 4;
		if (*at >= '0' && *at <= '9') {
			return (pid_t)atoi(at);
		}
	}
	return 0;
}

/* True once `pid` is gone; bounded, since what it is really asking is
 * whether the signal reached it at all. */
static bool process_gone(pid_t pid)
{
	for (int i = 0; i < 2000; i++) {
		if (kill(pid, 0) < 0 && errno == ESRCH) {
			return true;
		}
		usleep(1000);
	}
	return false;
}

/* C-c C-k signals the compilation's process *group*, so a command that
 * left something running in the background dies with it rather than
 * leaking a process kg can no longer name.  The group comes from the
 * shared spawner (src/process.c), which is the only reason this holds for
 * a shell command too. */
static void test_kill_compilation_reaches_grandchild(void)
{
	pid_t grandchild = 0;

	strcpy(g_next_command, "sleep 30 & echo PID=$!; exec sleep 30");
	editor_compile(0);

	for (int i = 0; i < 5000 && grandchild == 0; i++) {
		compilation_poll();
		grandchild = model_announced_pid();
		usleep(1000);
	}
	CHECK(grandchild > 0);
	CHECK(compilation_is_running());
	if (grandchild <= 0) {
		compilation_shutdown();
		return;
	}

	/* The first C-c C-k sends SIGINT, which kills the command itself but
	 * not the background job it left behind: a non-interactive shell
	 * starts one with SIGINT ignored, and while it holds the output pipe
	 * open the run cannot finish.  The second C-c C-k sends SIGKILL to
	 * the same group, which nothing can ignore. */
	editor_kill_compilation(0);
	for (int i = 0; i < 200 && compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}
	CHECK(compilation_is_running());
	editor_kill_compilation(0);
	for (int i = 0; i < 5000 && compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}

	CHECK(!compilation_is_running());
	CHECK(process_gone(grandchild));
	CHECK(strstr(g_model, "Compilation terminated by signal") != NULL);
}

int main(void)
{
	RUN(test_stream_seam_drives_parser);
	RUN(test_budget_charges_newlines);
	RUN(test_budget_bounds_pending_line);
	RUN(test_budget_boundary);
	RUN(test_stream_split_sequences);
	RUN(test_truncated_run_still_finishes);
	RUN(test_streaming_no_final_newline);
	RUN(test_resolve_null_filename);
	RUN(test_resolve_special_buffer);
	RUN(test_resolve_relative_no_slash);
	RUN(test_resolve_absolute);
	RUN(test_kill_compilation_reaches_grandchild);
	return test_summary();
}
