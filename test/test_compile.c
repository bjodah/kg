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
    = { KG_MODE_COMPILATION, "Compilation", NULL, "", NULL };

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

/* Set by the one case that needs a compilation which never starts: a
 * *compilation* buffer that cannot be prepared is the reachable half of
 * COMPILATION_DONE_SPAWN_FAILED. */
static int g_prepare_fails;

int buf_prepare_special_text(
    const char *name, struct editor_syntax *syntax, int readonly)
{
	(void)name;
	(void)syntax;
	(void)readonly;
	if (g_prepare_fails) {
		return -1;
	}
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

/* ------------------------- the programmatic seam ---------------------- */

/* What the completion callback saw.  `calls` is the assertion that matters
 * in every case: a caller is called back exactly once or not at all, and
 * never twice. */
static struct {
	int calls;
	struct compilation_result result;
	void *ctx;
} g_done;

static void note_done(const struct compilation_result *result, void *ctx)
{
	g_done.calls++;
	g_done.result = *result;
	g_done.ctx = ctx;
}

/* Poll the run AND the top-level delivery point, which is what the editor's
 * main loop does; bounded, so a case that would hang fails instead. */
static void pump_compilation(int milliseconds)
{
	const char *scale_text = getenv("KG_TEST_TIME_SCALE");
	double scale = scale_text != NULL ? atof(scale_text) : 1.0;
	int budget;

	if (scale < 1.0 || scale > 30.0) {
		scale = 1.0;
	}
	budget = (int)(milliseconds * scale);
	for (int i = 0; i < budget; i++) {
		compilation_poll();
		compilation_deliver_completion();
		if (!compilation_is_running() && g_done.calls > 0) {
			return;
		}
		usleep(1000);
	}
}

static unsigned start_programmatic(const char *command)
{
	unsigned generation = 0;
	char dir[PATH_MAX];

	memset(&g_done, 0, sizeof(g_done));
	if (!getcwd(dir, sizeof(dir))) {
		strcpy(dir, ".");
	}
	CHECK(compilation_start_programmatic(
		  command, dir, buf_handle(0), note_done, &g_done, &generation)
	    == COMPILATION_ACCEPTED);
	CHECK(generation != 0);
	return generation;
}

/* The exit code reaches the caller, exactly once -- and the ordinary
 * *compilation* behaviour is untouched: the same header, the same output,
 * the same finish line. */
static void test_programmatic_reports_the_exit_code(void)
{
	start_programmatic("printf 'built\\n'; exit 3");
	pump_compilation(5000);

	CHECK(g_done.calls == 1);
	CHECK(g_done.result.status == COMPILATION_DONE_EXITED);
	CHECK(g_done.result.exit_code == 3);
	CHECK(!g_done.result.truncated);
	CHECK(g_done.ctx == &g_done);
	CHECK(!compilation_is_running());
	CHECK(strstr(g_model, "Compilation started in ") != NULL);
	CHECK(strstr(g_model, "built") != NULL);
	CHECK(strstr(g_model, "Compilation finished with exit code 3") != NULL);
}

/* A compilation already running answers BUSY and asks nobody anything: the
 * confirmation prompt this file stubs would have said no in any case, and
 * the point is that it is never reached. */
static void test_programmatic_is_busy_rather_than_prompting(void)
{
	unsigned second = 1234;
	char dir[PATH_MAX];

	if (!getcwd(dir, sizeof(dir))) {
		strcpy(dir, ".");
	}
	start_programmatic("exec sleep 5");
	for (int i = 0; i < 200 && !compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}
	CHECK(compilation_is_running());
	CHECK(compilation_start_programmatic(
		  "true", dir, buf_handle(0), note_done, &g_done, &second)
	    == COMPILATION_BUSY);
	CHECK(second == 0);
	CHECK(g_done.calls == 0);

	/* C-c C-k ends it, and the run that was interrupted is reported as
	 * interrupted rather than as a success. */
	editor_kill_compilation(0);
	pump_compilation(5000);
	CHECK(g_done.calls == 1);
	CHECK((g_done.result.status == COMPILATION_DONE_SIGNALLED
		&& g_done.result.signal_number == SIGINT)
	    || (g_done.result.status == COMPILATION_DONE_EXITED
		&& g_done.result.exit_code == 0));
}

/* The callback runs at the top-level safe point and NOWHERE else:
 * compilation_poll() runs underneath minibuffer prompts, and a callback
 * that started a debug session from there would change buffers and windows
 * under an unrelated question. */
static void test_programmatic_completion_waits_for_the_safe_point(void)
{
	start_programmatic("exit 0");
	for (int i = 0; i < 5000 && compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}
	CHECK(!compilation_is_running());
	CHECK(g_done.calls == 0);
	compilation_deliver_completion();
	CHECK(g_done.calls == 1);
	CHECK(g_done.result.status == COMPILATION_DONE_EXITED);
	/* Delivering again delivers nothing: exactly once. */
	compilation_deliver_completion();
	CHECK(g_done.calls == 1);
}

/* Cancelling by generation drops the callback and leaves the RUN alone: the
 * user's compilation is not the caller's to kill, and *compilation* still
 * finishes as it always did. */
static void test_programmatic_cancellation_drops_only_the_callback(void)
{
	unsigned generation = start_programmatic("printf 'still here\\n'");

	compilation_cancel_programmatic(generation);
	for (int i = 0; i < 5000 && compilation_is_running(); i++) {
		compilation_poll();
		compilation_deliver_completion();
		usleep(1000);
	}
	CHECK(!compilation_is_running());
	CHECK(g_done.calls == 0);
	CHECK(strstr(g_model, "still here") != NULL);
	CHECK(strstr(g_model, "Compilation finished with exit code 0") != NULL);
	/* A generation that has been cancelled, and one that never existed,
	 * are both ignored rather than dropping somebody else's. */
	compilation_cancel_programmatic(generation);
	compilation_cancel_programmatic(0);
}

/* Nothing ever ran, and the caller still hears about it once: one
 * completion path, not two. */
static void test_programmatic_spawn_failure_is_a_completion(void)
{
	g_prepare_fails = 1;
	start_programmatic("true");
	g_prepare_fails = 0;
	CHECK(!compilation_is_running());
	CHECK(g_done.calls == 0);
	compilation_deliver_completion();
	CHECK(g_done.calls == 1);
	CHECK(g_done.result.status == COMPILATION_DONE_SPAWN_FAILED);
}

/* The editor is exiting with a callback still owed: it is delivered, so a
 * caller can release what it owns, rather than dropped. */
static void test_programmatic_shutdown_delivers_a_cancellation(void)
{
	start_programmatic("sleep 5");
	for (int i = 0; i < 200 && !compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}
	compilation_shutdown();
	CHECK(g_done.calls == 1);
	CHECK(g_done.result.status == COMPILATION_DONE_CANCELLED);
	CHECK(!compilation_is_running());
	/* And the slot is free again afterwards. */
	start_programmatic("true");
	pump_compilation(5000);
	CHECK(g_done.calls == 1);
}

/* ----------------------------- the output feed ------------------------ */

/* What the diagnostic hooks saw, which is the half of the feed that makes
 * C-x ` work rather than the half a reader sees. */
static struct feed_seen {
	int resets;
	char directory[PATH_MAX];
	int lines;
	char last_line[256];
	size_t last_start;
} feed_seen;

static void feed_reset_hook(struct kg_buffer_handle buffer,
    const char *initial_cwd, size_t initial_cwd_len)
{
	(void)buffer;
	feed_seen.resets++;
	snprintf(feed_seen.directory, sizeof(feed_seen.directory), "%.*s",
	    (int)initial_cwd_len, initial_cwd);
}

static void feed_line_hook(const char *line, size_t len, size_t line_start_pos)
{
	feed_seen.lines++;
	snprintf(feed_seen.last_line, sizeof(feed_seen.last_line), "%.*s",
	    (int)len, line);
	feed_seen.last_start = line_start_pos;
}

static const struct compile_diag_hooks feed_hooks
    = { feed_reset_hook, feed_line_hook };

/* Bytes somebody else collected, arriving in the shape they really arrive
 * in: a diagnostic split across two deliveries, and a last line with no
 * newline on it because the producer stopped there.  Both are what a
 * protocol `output` event does, and a feed that treated one delivery as one
 * line would print a transcript neither the compiler nor the user wrote. */
static void test_the_feed_turns_bytes_into_a_compilation(void)
{
	memset(&feed_seen, 0, sizeof(feed_seen));
	compilation_set_diag_hooks(&feed_hooks);
	CHECK(compilation_feed_begin("Go (delve)", "/w/example"));
	CHECK(strstr(g_model, "Compilation started in /w/example") != NULL);
	CHECK(strstr(g_model, "$ Go (delve)") != NULL);
	CHECK(feed_seen.resets == 1);
	CHECK(strcmp(feed_seen.directory, "/w/example") == 0);
	compilation_feed_bytes("# example/m\n./main.go:6:2: syntax ", 34);
	compilation_feed_bytes("error: unexpected }", 19);
	CHECK(feed_seen.lines == 1);
	compilation_feed_finish("Launch failed: Go (delve)");
	/* The unterminated last line was committed, exactly once. */
	CHECK(feed_seen.lines == 2);
	CHECK(strcmp(feed_seen.last_line,
		  "./main.go:6:2: syntax error: unexpected }")
	    == 0);
	CHECK(strstr(g_model, "./main.go:6:2: syntax error: unexpected }\n")
	    != NULL);
	CHECK(strstr(g_model, "Launch failed: Go (delve)") != NULL);
	compilation_set_diag_hooks(NULL);
}

/* The relative forms a compiler really emits, with and without the leading
 * `./`: both are committed verbatim, because resolving them against the
 * directory is the parser's job and rewriting them here would break it. */
static void test_the_feed_keeps_relative_paths_verbatim(void)
{
	memset(&feed_seen, 0, sizeof(feed_seen));
	compilation_set_diag_hooks(&feed_hooks);
	CHECK(compilation_feed_begin("build", "/w"));
	compilation_feed_bytes("./a.go:1:1: bad\nb/c.go:2:3: worse\n", 34);
	CHECK(feed_seen.lines == 2);
	CHECK(strcmp(feed_seen.last_line, "b/c.go:2:3: worse") == 0);
	CHECK(strstr(g_model, "./a.go:1:1: bad\n") != NULL);
	CHECK(strstr(g_model, "b/c.go:2:3: worse\n") != NULL);
	compilation_feed_finish("done");
	compilation_set_diag_hooks(NULL);
}

/* "Hold it until we know" with no bound is a chatty adapter's memory leak,
 * so the feed is charged against the same retained-output budget an
 * ordinary compilation is -- and says so in the buffer when it bites. */
static void test_the_feed_respects_the_output_budget(void)
{
	char noise[512];

	memset(noise, 'x', sizeof(noise));
	compilation_set_maximum_output(64);
	CHECK(compilation_feed_begin("build", "/w"));
	compilation_feed_bytes(noise, sizeof(noise));
	compilation_feed_bytes("\n", 1);
	compilation_feed_finish("done");
	CHECK(strstr(g_model, "output truncated after 64 bytes") != NULL);
	compilation_set_maximum_output(8 * 1024 * 1024);
}

/* The user's own build owns *compilation*.  A debugger that took it would
 * destroy a running compilation's transcript, so the feed is refused and
 * the caller keeps its bytes. */
static void test_a_running_compilation_refuses_the_feed(void)
{
	start_programmatic("sleep 5");
	for (int i = 0; i < 200 && !compilation_is_running(); i++) {
		compilation_poll();
		usleep(1000);
	}
	CHECK(compilation_is_running());
	CHECK(!compilation_feed_begin("build", "/w"));
	editor_kill_compilation(0);
	pump_compilation(5000);
	CHECK(!compilation_is_running());
	/* And once it is over the feed is available again. */
	CHECK(compilation_feed_begin("build", "/w"));
	compilation_feed_finish("done");
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
	RUN(test_programmatic_reports_the_exit_code);
	RUN(test_programmatic_is_busy_rather_than_prompting);
	RUN(test_programmatic_completion_waits_for_the_safe_point);
	RUN(test_programmatic_cancellation_drops_only_the_callback);
	RUN(test_programmatic_spawn_failure_is_a_completion);
	RUN(test_programmatic_shutdown_delivers_a_cancellation);
	RUN(test_the_feed_turns_bytes_into_a_compilation);
	RUN(test_the_feed_keeps_relative_paths_verbatim);
	RUN(test_the_feed_respects_the_output_budget);
	RUN(test_a_running_compilation_refuses_the_feed);
	return test_summary();
}
