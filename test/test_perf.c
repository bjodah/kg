/* test_perf.c — what the editor's hot paths cost, in counters.
 *
 * Every phase of the measured performance work, currently
 * doc/plans/2026-07-31-follow-ups/07-visual-line-geometry-index.md,
 * has to name the evidence that justifies it before it may change code.
 * This file is that evidence, and it is deterministic: a counter says the
 * same thing on a loaded box, inside a sanitizer lane and under valgrind,
 * where a wall time says whatever the machine felt like.  `make bench`
 * reports times; this reports shapes, and only this is a gate.
 *
 * Assertions here are written as bounds ("no more than c*log2(rows)
 * reallocations"), not as equalities, except where the exact number is
 * the property (one row update per logical replacement).  A test that
 * pins an exact allocation count fails on every unrelated refactor and
 * gets deleted; a bound survives and still catches the pathology.
 *
 * This binary links every editor translation unit except main.c, built
 * with -DKG_PERF_COUNTERS=1, so the counters measure the real code.
 */

#include "../src/compile.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "../src/lisp.h"
#ifdef KG_USE_LSP
#include "../src/lsp_transport.h"
#include "../src/process.h"
#endif
#include "../src/syntax.h"
#include "../src/tty.h"
#include "../src/vgeom.h"
#include "test.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- Helpers ---- */

static void setup(void)
{
	/* win_init() below is the real winmgr.c/bufmgr.c pair (this binary
	 * links everything but main.c), so it is a real KG_EVENT_VIEW_ATTACHED
	 * producer every one of this file's many tests runs through; a drain
	 * with nobody subscribed -- the same relief src/main.c's safe points
	 * give a real session -- is what keeps a later test's window attach
	 * from being refused by an earlier test's undrained events, a
	 * refusal that would leave that test's window not showing its
	 * buffer at all, not just short one event. */
	kg_event_drain_safe();
	/* Before reset_current_buffer(), which memsets the record: a backend
	 * that keeps a parse tree keeps it behind a pointer that lives in
	 * there, and zeroing the field leaks it -- which is what
	 * LeakSanitizer says about a tree-sitter build of this binary. */
	syntax_state_release(bcur());
	free_all_rows();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	wcur()->h = 24;
	wcur()->w = 80;
	bcur()->readonly_override = -1;
	win_total_rows = 24;
	win_total_cols = 80;
	win_init();
	running = 1;
	undo_free();
	undo_init();
	kg_perf_reset();
}

static void teardown(void)
{
	syntax_state_release(bcur());
	free_all_rows();
	bcur()->row = NULL;
	bcur()->numrows = 0;
	undo_free();
	/* win_init() (setup(), above) frees a stale index at the start of
	 * the next test; this also frees it at the end of this one, so the
	 * last test in the binary does not leave one for LeakSanitizer to
	 * find at process exit. */
	vgeom_window_free(wcur());
}

static unsigned long long counter(enum kg_perf_counter c)
{
	return kg_perf_read(c);
}

/* An isolated XDG config root, for the load-time counters below: they
 * time a *user's* init file and the packages it requires, and there is no
 * way to have either without a config directory to put them in.  Same
 * shape as test_lisp.c's own setup_config_root/remove_config_root pair;
 * not shared with it because the two binaries link different objects. */
static int perf_config_root(char *root, size_t rootsize)
{
	char path[512];

	(void)snprintf(root, rootsize, "/tmp/kg-perf-cfg-XXXXXX");
	if (!mkdtemp(root)) {
		return 1;
	}
	(void)snprintf(path, sizeof(path), "%s/kg", root);
	if (mkdir(path, 0700) != 0) {
		return 1;
	}
	(void)snprintf(path, sizeof(path), "%s/kg/lisp", root);
	if (mkdir(path, 0700) != 0) {
		return 1;
	}
	return setenv("XDG_CONFIG_HOME", root, 1) != 0;
}

static int perf_write_file(const char *path, const char *text)
{
	FILE *file = fopen(path, "wb");

	if (!file) {
		return 1;
	}
	(void)fputs(text, file);
	return fclose(file) != 0;
}

static void perf_remove_config_root(const char *root)
{
	static const char *const entries[] = {
		"%s/kg/init.el",
		"%s/kg/lisp/perfpkg.el",
		"%s/kg/lisp/perfpkg2.el",
		"%s/kg/lisp/perfouter.el",
		"%s/kg/lisp/perfinner.el",
		"%s/kg/lisp",
		"%s/kg",
		"%s",
	};
	char path[512];
	size_t i;

	for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		(void)snprintf(path, sizeof(path), entries[i], root);
		(void)remove(path);
	}
	(void)unsetenv("XDG_CONFIG_HOME");
}

/* Number of times a doubling array has to grow to hold `n` records, plus
 * slack for the first few small steps.  The point of the bound is that it
 * is logarithmic; the constant is not load-bearing. */
static unsigned long long log_growth_bound(int n)
{
	unsigned long long steps = 0;
	long long cap = 1;

	while (cap < n) {
		cap *= 2;
		steps++;
	}
	return steps + 4;
}

/* editor_refresh_screen() writes the frame to fd 1.  Send it to /dev/null
 * so the suite's own output stays readable, and so the counters below
 * measure a full repaint rather than a pipe's mood. */
static void refresh_quietly(void)
{
	int saved = dup(STDOUT_FILENO);
	int devnull = open("/dev/null", O_WRONLY);

	fflush(stdout);
	if (devnull >= 0) {
		dup2(devnull, STDOUT_FILENO);
	}
	editor_refresh_screen();
	fflush(stdout);
	if (saved >= 0) {
		dup2(saved, STDOUT_FILENO);
		close(saved);
	}
	if (devnull >= 0) {
		close(devnull);
	}
}

/* Write `lines` short lines to a fresh temporary file.  Returns 0 on
 * success and fills `path`. */
static int write_lines_file(char *path, size_t path_size, int lines)
{
	int fd, i;
	FILE *fp;

	snprintf(path, path_size, "test_perf_load_XXXXXX");
	fd = mkstemp(path);
	if (fd < 0) {
		return -1;
	}
	fp = fdopen(fd, "w");
	if (!fp) {
		close(fd);
		unlink(path);
		return -1;
	}
	for (i = 0; i < lines; i++) {
		fprintf(fp, "line %d of the corpus\n", i);
	}
	fclose(fp);
	return 0;
}

/* Whether the compiled-in syntax backend keeps per-buffer state for a mode
 * it highlights.  The legacy scanners derive every row from the row above
 * it and keep none, so what a load transfers is NULL; a parsing backend
 * keeps its parser and its tree.  Which of the two is compiled in is a
 * source-list decision (src/syntax_backend.h), and this is the one place
 * this suite has to know about it. */
#ifdef KG_USE_TREE_SITTER
#define BACKEND_KEEPS_STATE 1
#else
#define BACKEND_KEEPS_STATE 0
#endif

/* ---- File loader row-array growth ---- */

static void test_load_row_array_growth(void)
{
	char path[64];
	struct temp_load_result res;
	const int lines = 4096;

	setup();
	if (write_lines_file(path, sizeof(path), lines) != 0) {
		CHECK(0 && "could not create a temporary file");
		teardown();
		return;
	}
	kg_perf_reset();
	CHECK(load_file_transactional(path, &res) == 0);
	/* A file ending in a newline stages one more row: the empty last
	 * line the newline opens. */
	CHECK(res.numrows == lines + 1);
	/* The staged row array doubles, like the live one.
	 * It used to realloc to an exact size once per line, so loading R
	 * lines copied O(R^2) row records. */
	CHECK(counter(KG_PERF_ROW_ARRAY_GROW) <= log_growth_bound(lines));
	/* One render and one highlight per staged row, not one per row per
	 * line read, and not a second pass on commit. */
	CHECK(counter(KG_PERF_ROW_UPDATE) == (unsigned long long)lines + 1);
	CHECK(counter(KG_PERF_SYNTAX_ROW) == (unsigned long long)lines + 1);
	free_load_result(&res);
	unlink(path);
	teardown();
}

/* Comment-heavy C: an unterminated block comment sets hl_oc, which is the
 * state the next row reads, and an unterminated string does not. */
static void write_comment_c(FILE *fp)
{
	int i;

	for (i = 0; i < 40; i++) {
		fprintf(fp, "/* a comment opened on this row\n");
		fprintf(fp, " * and continued across this one%s\n",
		    i % 3 ? "" : " */");
		fprintf(fp, "int v%d = %d; /* trailing */ \"str\"\n", i, i);
		fprintf(fp, "#define M%d \"unterminated /* inside\n", i);
	}
}

/* Markdown, which is not the same shape at all.  The generic scanner
 * reads only the row above; markdown_syntax() also looks one row *down*
 * (a setext heading is one, if the row under it is its underline) and
 * writes one row *up* (the underline re-highlights the heading above it).
 * The staged pass publishes rows one at a time -- bcur()->numrows is the
 * count staged so far -- so during it the look-down is out of bounds and
 * the heading is coloured only when its underline arrives and reaches
 * back for it.  A blank line inside a fenced block is the other shape
 * this adds: rsize 0 with a highlight callback installed, which is the
 * branch that propagates state without touching any bytes. */
static void write_markdown(FILE *fp)
{
	int i;

	for (i = 0; i < 30; i++) {
		fprintf(fp, "A setext heading %d\n", i);
		fprintf(fp, "==========\n");
		fprintf(fp, "text with `code` and *emphasis*\n");
		fprintf(fp, "```\n");
		fprintf(fp, "fenced line %d\n", i);
		fprintf(fp, "\n");
		fprintf(fp, "still inside the fence\n");
		/* Every fourth block is left open, so the fence that opens
		 * the next one closes it instead and the states interleave. */
		fprintf(fp, "%s\n", i % 4 ? "```" : "no fence here");
	}
}

/* The staged pass is the only highlight pass a load pays for now, so it
 * has to leave the buffer in the state a from-scratch rehighlight would:
 * same hl bytes, same hl_oc, row for row.
 *
 * That is also this suite's equivalence proof for the render/prepare split
 * (doc/plans/kg-tree-sitter-plan.md, Phase 4): the load no longer renders
 * and colours a row at a time but renders every row, then colours the whole
 * staged document in one preparation pass, and this differential is what
 * says the two orders settle at the same colours. */
static void check_load_highlight_is_final(const char *name_template,
    int suffix_len, void (*write_corpus)(FILE *), int expect_state)
{
	char path[64];
	unsigned char **hl = NULL;
	int *oc = NULL;
	int fd, i, rows;
	FILE *fp;

	setup();
	snprintf(path, sizeof(path), "%s", name_template);
	fd = mkstemps(path, suffix_len);
	CHECK(fd >= 0);
	if (fd < 0) {
		teardown();
		return;
	}
	fp = fdopen(fd, "w");
	CHECK(fp != NULL);
	if (!fp) {
		close(fd);
		unlink(path);
		teardown();
		return;
	}
	write_corpus(fp);
	fclose(fp);

	CHECK(editor_open(path) == 0);
	rows = bcur()->numrows;
	CHECK(rows > 100);
	/* The load prepared whatever the backend keeps against the staged
	 * rows and transferred it here (commit_load_result()) -- the
	 * invariant that says the transfer happened rather than something
	 * else being left behind.  `expect_state` is what the caller knows:
	 * whether this backend keeps state at all, and whether this corpus's
	 * mode is one it can highlight. */
	CHECK((bcur()->syntax_state != NULL) == expect_state);
#ifdef KG_USE_TREE_SITTER
	/* A load is ONE parse of the whole document, and it is the parse
	 * from nothing -- made once inside the load transaction against the
	 * staged rows (syntax_backend_prepare()), with nothing reparsing it
	 * on publication and no row repainted by the edit path, which has
	 * not run.  A corpus whose mode has no grammar (the .md caller) is
	 * parsed not at all, which is the same statement with the count at
	 * zero. */
	CHECK(counter(KG_PERF_TS_PARSE) == (unsigned long long)expect_state);
	CHECK(
	    counter(KG_PERF_TS_FULL_PARSE) == (unsigned long long)expect_state);
	CHECK(counter(KG_PERF_TS_REHIGHLIGHT_ROW) == 0);
#endif
	hl = calloc((size_t)rows, sizeof(*hl));
	oc = calloc((size_t)rows, sizeof(*oc));
	CHECK(hl != NULL && oc != NULL);
	if (hl && oc) {
		for (i = 0; i < rows; i++) {
			int n = bcur()->row[i].rsize;

			oc[i] = bcur()->row[i].hl_oc;
			hl[i] = n > 0 ? malloc((size_t)n) : NULL;
			if (hl[i]) {
				memcpy(hl[i], bcur()->row[i].hl, (size_t)n);
			}
		}
		editor_rehighlight_all(bcur());
		for (i = 0; i < rows; i++) {
			if (hl[i]) {
				CHECK(memcmp(hl[i], bcur()->row[i].hl,
					  (size_t)bcur()->row[i].rsize)
				    == 0);
				free(hl[i]);
				hl[i] = NULL;
			}
			CHECK(oc[i] == bcur()->row[i].hl_oc);
		}
	}
	free(hl);
	free(oc);
	free(bcur()->filename);
	bcur()->filename = NULL;
	unlink(path);
	teardown();
}

static void test_load_highlight_is_final(void)
{
	check_load_highlight_is_final(
	    "test_perf_hl_XXXXXX.c", 2, write_comment_c, BACKEND_KEEPS_STATE);
	/* Markdown, which the legacy scanner reads one row DOWN for and the
	 * tree-sitter backend parses like any other language: batch 1
	 * registered its (block) grammar, so under that backend this corpus
	 * now has the same load shape as the C one -- one parse from
	 * nothing, inside the load transaction, and no row repainted after
	 * it. */
	check_load_highlight_is_final(
	    "test_perf_hl_XXXXXX.md", 3, write_markdown, BACKEND_KEEPS_STATE);
}

/* ---- Live row-array growth ---- */

static void test_insert_row_array_growth(void)
{
	const int rows = 4096;
	int i;

	setup();
	kg_perf_reset();
	for (i = 0; i < rows; i++) {
		editor_insert_row(bcur(), i, "x", 1);
	}
	CHECK(bcur()->numrows == rows);
	/* The growth path every subprocess output line
	 * takes doubles, so R rows cost O(log R) reallocations.  It used to
	 * realloc to an exact size once per row -- 4096 of them here. */
	CHECK(counter(KG_PERF_ROW_ARRAY_GROW) <= log_growth_bound(rows));
	teardown();
}

/* The row array a build log fills: compilation and shell-command output
 * reaches a buffer through buf_append_special_text(), one
 * editor_insert_row(bcur(), ) per line, on a path the user watches in real
 * time. */
static void test_special_text_append_growth(void)
{
	const int lines = 20000;
	int slot, i;
	size_t len = 0;
	char *text = malloc((size_t)lines * 32 + 1);

	CHECK(text != NULL);
	if (!text) {
		return;
	}
	for (i = 0; i < lines; i++) {
		len += (size_t)snprintf(
		    text + len, 32, "cc -c file%05d.c\n", i);
	}

	setup();
	slot = buf_prepare_special_text("*perf-output*", NULL, 1);
	CHECK(slot >= 0);
	if (slot >= 0) {
		kg_perf_reset();
		CHECK(buf_append_special_text(slot, text, len) == 0);
		CHECK(
		    counter(KG_PERF_ROW_ARRAY_GROW) <= log_growth_bound(lines));
		buf_clear_special_text(slot);
		free(buflist[slot].filename);
		buflist[slot].filename = NULL;
		buflist[slot].active = 0;
		buf_count--;
	}
	free(text);
	teardown();
}

/* What a poll of subprocess output costs the buffer it mirrors into.
 *
 * Every read chunk truncates the still-open line off the last row and
 * re-mirrors it, so a long line arriving in small reads is re-rendered
 * once per read.  This measures the number a future coalescing change
 * has to improve before it is allowed to add state. */
static void test_compilation_mirror_updates_per_read(void)
{
	struct compilation_state s;
	const int chunk = 64;
	const int line = 4096;
	int slot, i;
	char *bytes = malloc((size_t)line + 1);

	CHECK(bytes != NULL);
	if (!bytes) {
		return;
	}
	memset(bytes, 'o', (size_t)line);
	bytes[line] = '\n';

	setup();
	slot = buf_prepare_special_text("*perf-compile*", NULL, 1);
	CHECK(slot >= 0);
	if (slot >= 0) {
		memset(&s, 0, sizeof(s));
		compilation_stream_reset(&s, (size_t)line * 4);
		s.compilation_buffer = buf_handle(slot);
		kg_perf_reset();
		for (i = 0; i + chunk <= line + 1; i += chunk) {
			compilation_process_bytes(&s, bytes + i, chunk);
		}
		/* One truncate and one re-mirror of the whole pending line
		 * per read: 64 reads of a 4 KiB line cost 128 row updates,
		 * and the bytes each re-renders grow with the line. */
		CHECK(counter(KG_PERF_ROW_UPDATE)
		    == (unsigned long long)2 * (line / chunk));
		compilation_stream_reset(&s, 0);
		buf_clear_special_text(slot);
		free(buflist[slot].filename);
		buflist[slot].filename = NULL;
		buflist[slot].active = 0;
		buf_count--;
	}
	free(bytes);
	teardown();
}

/* ---- The LSP transport's inbox ---- */

#ifdef KG_USE_LSP

/* Several complete frames in one write() cost exactly one read().
 *
 * The property is the inbox's whole design -- reads append to it and
 * frames are parsed out of it, so "four messages arrived in one read" and
 * "a header split across three reads" are the same code path
 * (src/framed_io.c's framing contract) -- and it is a cost rather than
 * a behaviour: test_lsp_transport.c's test_several_messages_from_one_read()
 * passes just as well with LSP_TRANSPORT_READ_CHUNK set to a single byte,
 * because the parser cannot tell where the reads fell and the transport's
 * API does not report them.  A counter can.
 *
 * The child writes its three frames with one printf, so the reader sees
 * either nothing yet (EAGAIN, which is not a read that returned bytes) or
 * all of them: the count is one, not "one or two on a fast box". */
static void test_batched_frames_cost_one_read(void)
{
	const char *argv[4] = { "/bin/sh", "-c",
		"printf 'Content-Length: 3\\r\\n\\r\\none"
		"Content-Length: 3\\r\\n\\r\\ntwo"
		"Content-Length: 5\\r\\n\\r\\nthree'; sleep 2",
		NULL };
	struct kg_spawn_request req = { .argv = argv, .stdin_fd = -1 };
	struct timespec nap = { 0, 1000000 }; /* 1 ms */
	struct lsp_transport *t;
	const char *body = NULL;
	size_t len = 0;
	int taken = 0;
	int spins;

	kg_perf_reset();
	t = lsp_transport_start_wire(&req, LSP_WIRE_STDIO);
	CHECK(t != NULL);
	if (!t) {
		return;
	}
	/* Bounded rather than timed: 5000 ms of 1 ms naps is a hang
	 * reported as a failure, not a suite that waits forever. */
	for (spins = 0; taken < 3 && spins < 5000; spins++) {
		while (lsp_transport_next_message(t, &body, &len) == 1) {
			taken++;
		}
		if (taken < 3) {
			nanosleep(&nap, NULL);
		}
	}
	CHECK(taken == 3);
	CHECK(counter(KG_PERF_LSP_INBOX_READ) == 1);
	lsp_transport_close(t);
}

#endif /* KG_USE_LSP */

/* ---- The editor's idle wait ---- */

/* The three cases below are the evidence for the input-loop follow-up
 * (commit 706030d's last paragraph): a language server's output is drained
 * when the child writes it, not once per idle tick.
 *
 * A pipe is the right stand-in for both descriptors here.  The editor's
 * wait cannot tell a terminal from a pipe -- it asks poll(2), which
 * answers the same for both -- and what the announce channel actually IS
 * is a pipe, one pipe-load at a time.  So the numbers these assert are the
 * numbers the editor gets: KG_PERF_IDLE_FD_WAKE per pipe-load, and a tick
 * count that does not depend on how much the child wrote.  Measured end to
 * end on a 512 KiB pre-announce banner, before and after: 76 ticks and
 * 6.95 s became 7 ticks, 64 fd wakes and 0.041 s. */

/* Two pipes: `tty` stands in for the terminal, `child` for a server. */
struct idle_fixture {
	int tty[2];
	int child[2];
};

static int idle_fixture_open(struct idle_fixture *f)
{
	if (pipe(f->tty) != 0) {
		return 0;
	}
	if (pipe(f->child) != 0) {
		close(f->tty[0]);
		close(f->tty[1]);
		return 0;
	}
	return 1;
}

static void idle_fixture_close(struct idle_fixture *f)
{
	close(f->tty[0]);
	close(f->tty[1]);
	close(f->child[0]);
	close(f->child[1]);
}

/* A descriptor with bytes on it ends the wait, and is counted as the
 * thing that ended it.  This is the whole of the change: before it, the
 * only thing that could end a wait was the tick, so a pipe-load of a
 * server's log waited out 100 ms whether or not it had already arrived. */
static void test_idle_wait_ends_on_a_ready_descriptor(void)
{
	struct idle_fixture f;
	int extra;

	if (!idle_fixture_open(&f)) {
		CHECK(0);
		return;
	}
	extra = f.child[0];
	CHECK(write(f.child[1], "x", 1) == 1);
	kg_perf_reset();
	/* A tick of 5000 ms that is never reached: a wait this returns from
	 * at once cannot be a timeout the test slept through. */
	CHECK(kg_idle_wait(f.tty[0], &extra, 1, 5000) == KG_IDLE_FD);
	CHECK(counter(KG_PERF_IDLE_WAIT) == 1);
	CHECK(counter(KG_PERF_IDLE_FD_WAKE) == 1);
	CHECK(counter(KG_PERF_IDLE_TICK) == 0);
	idle_fixture_close(&f);
}

/* And with nothing ready it still ends on its tick, which is what keeps
 * the auto-revert poll, a running compilation and the process table at the
 * cadence they had before any of this: a descriptor only ever ends a wait
 * EARLY. */
static void test_idle_wait_ends_on_its_tick(void)
{
	struct idle_fixture f;
	int extra;

	if (!idle_fixture_open(&f)) {
		CHECK(0);
		return;
	}
	extra = f.child[0];
	kg_perf_reset();
	CHECK(kg_idle_wait(f.tty[0], &extra, 1, 0) == KG_IDLE_TICK);
	CHECK(counter(KG_PERF_IDLE_WAIT) == 1);
	CHECK(counter(KG_PERF_IDLE_TICK) == 1);
	CHECK(counter(KG_PERF_IDLE_FD_WAKE) == 0);
	idle_fixture_close(&f);
}

/* The shape the follow-up bought, stated as a ratio: N pipe-loads of a
 * server's pre-announce log cost N wakes and NO ticks.  Before, the same N
 * pipe-loads cost N ticks -- N * 100 ms of latency before the editor could
 * connect, which is where six seconds for a 512 KiB banner came from.
 *
 * The tick here is 5000 ms and the loop must never reach it: any tick at
 * all would mean a pipe-load the wait did not hear about. */
#define IDLE_PIPE_LOADS 32
#define IDLE_PIPE_LOAD_BYTES 512

static void test_pre_announce_pipe_loads_cost_wakes_not_ticks(void)
{
	char chunk[IDLE_PIPE_LOAD_BYTES];
	struct idle_fixture f;
	int extra;
	int i;

	if (!idle_fixture_open(&f)) {
		CHECK(0);
		return;
	}
	extra = f.child[0];
	memset(chunk, 'a', sizeof(chunk));
	kg_perf_reset();
	for (i = 0; i < IDLE_PIPE_LOADS; i++) {
		CHECK(write(f.child[1], chunk, sizeof(chunk))
		    == (ssize_t)sizeof(chunk));
		if (kg_idle_wait(f.tty[0], &extra, 1, 5000) != KG_IDLE_FD) {
			break;
		}
		CHECK(read(f.child[0], chunk, sizeof(chunk))
		    == (ssize_t)sizeof(chunk));
	}
	CHECK(counter(KG_PERF_IDLE_FD_WAKE) == IDLE_PIPE_LOADS);
	CHECK(counter(KG_PERF_IDLE_WAIT) == IDLE_PIPE_LOADS);
	CHECK(counter(KG_PERF_IDLE_TICK) == 0);
	idle_fixture_close(&f);
}

/* A keystroke beats a ready server, because a user waiting on their own
 * key is the one thing an editor may not make wait.  Both are ready here
 * and the verdict is the terminal's; the server's descriptor is still
 * ready afterwards and the next wait says so. */
static void test_idle_wait_prefers_the_terminal(void)
{
	struct idle_fixture f;
	int extra;

	if (!idle_fixture_open(&f)) {
		CHECK(0);
		return;
	}
	extra = f.child[0];
	CHECK(write(f.tty[1], "k", 1) == 1);
	CHECK(write(f.child[1], "x", 1) == 1);
	kg_perf_reset();
	CHECK(kg_idle_wait(f.tty[0], &extra, 1, 5000) == KG_IDLE_KEY);
	CHECK(counter(KG_PERF_IDLE_WAIT) == 1);
	CHECK(counter(KG_PERF_IDLE_FD_WAKE) == 0);
	CHECK(counter(KG_PERF_IDLE_TICK) == 0);
	idle_fixture_close(&f);
}

/* ---- Screen append buffer ---- */

static void refresh_ab_shape(int screen_rows, int screen_cols,
    unsigned long long *appends, unsigned long long *grows,
    unsigned long long *copied, unsigned long long *bytes)
{
	int i;

	setup();
	win_total_rows = screen_rows;
	win_total_cols = screen_cols;
	win_init();
	for (i = 0; i < screen_rows; i++) {
		editor_insert_row(bcur(), i, "some ordinary buffer text", 25);
	}
	kg_perf_reset();
	refresh_quietly();
	*appends = counter(KG_PERF_AB_APPEND);
	*grows = counter(KG_PERF_AB_GROW);
	*copied = counter(KG_PERF_AB_COPIED);
	*bytes = counter(KG_PERF_AB_BYTES);
	teardown();
}

static void test_frame_append_growth(void)
{
	unsigned long long appends, grows, copied, bytes;

	refresh_ab_shape(24, 80, &appends, &grows, &copied, &bytes);
	CHECK(appends > 100);
	/* The frame buffer has a capacity and doubles, so
	 * one repaint is a handful of reallocations instead of one per
	 * append -- which is what it used to be, hundreds of them, each
	 * copying the frame so far.  A 24x80 frame fits the first
	 * allocation outright. */
	CHECK(grows <= log_growth_bound((int)bytes));
	CHECK(copied <= 4 * bytes);

	refresh_ab_shape(200, 600, &appends, &grows, &copied, &bytes);
	CHECK(appends > 1000);
	CHECK(grows <= log_growth_bound((int)bytes));
	CHECK(copied <= 4 * bytes);
}

/* ---- Render and highlight storage ---- */

static void test_long_row_update_allocations(void)
{
	const int len = 1 << 20;
	char *line = malloc((size_t)len + 1);

	CHECK(line != NULL);
	if (!line) {
		return;
	}
	memset(line, 'x', (size_t)len);
	line[len] = '\0';

	setup();
	editor_insert_row(bcur(), 0, line, (size_t)len);
	kg_perf_reset();
	editor_update_row(bcur(), &bcur()->row[0]);
	CHECK(counter(KG_PERF_ROW_UPDATE) == 1);
	/* Render and highlight storage carry capacities,
	 * so re-updating a row no wider than it has been costs no
	 * allocation.  It used to free and malloc a fresh 1 MiB render, and
	 * realloc a 1 MiB highlight array, on every call. */
	CHECK(counter(KG_PERF_RENDER_ALLOC) == 0);
	CHECK(counter(KG_PERF_RENDER_BYTES) == 0);
	CHECK(counter(KG_PERF_HL_ALLOC) == 0);
	CHECK(bcur()->row[0].rsize == len);
	CHECK(bcur()->row[0].render[len] == '\0');

	teardown();
	free(line);
}

/* Storage is grown with slack once a row is long enough to be worth it,
 * so lengthening a row a byte at a time -- which is what typing into it
 * does -- does not reallocate per keystroke.  A 4 KiB row rather than
 * the 1 MiB one above: this loop rebuilds the render every iteration,
 * which is the editor's real per-keystroke cost and not something to pay
 * a thousand megabytes of under valgrind. */
static void test_typing_into_long_row_reuses_storage(void)
{
	const int len = 4096;
	const int typed = 400;
	char *line = malloc((size_t)len + 1);
	int i;

	CHECK(line != NULL);
	if (!line) {
		return;
	}
	memset(line, 'x', (size_t)len);
	line[len] = '\0';

	setup();
	editor_insert_row(bcur(), 0, line, (size_t)len);
	kg_perf_reset();
	for (i = 0; i < typed; i++) {
		editor_row_insert_char(&bcur()->row[0], 0, 'y');
	}
	CHECK(counter(KG_PERF_ROW_UPDATE) == (unsigned long long)typed);
	CHECK(counter(KG_PERF_RENDER_ALLOC) == 0);
	CHECK(counter(KG_PERF_HL_ALLOC) == 0);
	CHECK(bcur()->row[0].size == len + typed);
	teardown();
	free(line);
}

/* What one typed byte costs in a 1 MiB row when it goes through the edit
 * transaction, which is the editor's hottest path into it: self-insert,
 * backspace and forward-delete are all one-row replacements.
 *
 * The row is replaced in place and is the same width either way, so its
 * render and highlight storage is derived state the edit has no reason
 * to build again, and does not: the commit hands both to the row that
 * replaces it, capacities included, and the update refills them without
 * allocating.  It used to free the pair with the row and rebuild both
 * from nothing -- two megabytes of allocation and copying per
 * keystroke, on a row that already had the storage. */
static void test_one_byte_edit_in_long_row_derived_storage(void)
{
	const int len = 1 << 20;
	char *line = malloc((size_t)len + 1);

	CHECK(line != NULL);
	if (!line) {
		return;
	}
	memset(line, 'x', (size_t)len);
	line[len] = '\0';

	setup();
	editor_insert_row(bcur(), 0, line, (size_t)len);
	kg_perf_reset();
	CHECK(editor_row_replace_range(0, 0, 0, "y", 1, KG_EDIT_USER)
	    == 1); /* typed */
	CHECK(editor_row_replace_range(0, 0, 1, "", 0, KG_EDIT_USER)
	    == 1); /* erased */

	CHECK(counter(KG_PERF_ROW_UPDATE) == 2);
	CHECK(counter(KG_PERF_RENDER_ALLOC) == 0);
	CHECK(counter(KG_PERF_RENDER_BYTES) == 0);
	CHECK(counter(KG_PERF_HL_ALLOC) == 0);
	CHECK(bcur()->row[0].size == len);
	CHECK(bcur()->row[0].rsize == len);
	CHECK(bcur()->row[0].render[len] == '\0');
	teardown();
	free(line);
}

/* ---- One row update per logical replacement ---- */

static void test_replace_range_updates_once(void)
{
	setup();
	editor_insert_row(bcur(), 0, "aaaaaaaaaaaaaaaa", 16);
	kg_perf_reset();
	CHECK(editor_row_replace_range(0, 4, 8, "REPLACEMENT", 11, KG_EDIT_USER)
	    == 1);
	CHECK(counter(KG_PERF_ROW_UPDATE) == 1);
	CHECK(counter(KG_PERF_UNDO_PUSH) == 1);
	CHECK(bcur()->row[0].size == 19);
	teardown();
}

static void test_rect_delete_updates_per_byte(void)
{
	int r;

	setup();
	for (r = 0; r < 8; r++) {
		editor_insert_row(bcur(), r, "0123456789abcdef", 16);
	}
	CHECK(test_set_mark(bcur(), 0, 2));
	wcur()->cy = 7;
	wcur()->cx = 12;
	bcur()->rect_mode = 1;
	kg_perf_reset();
	editor_delete_rect();
	CHECK(bcur()->row[0].size == 6);
	/* One row rebuild per row, where rect.c used to
	 * delete a byte at a time and pay a full render and highlight
	 * rebuild for each -- 80 of them for this 8x10 rectangle. */
	CHECK(counter(KG_PERF_ROW_UPDATE) == 8);
	/* And still exactly one undo step for the whole rectangle. */
	CHECK(counter(KG_PERF_UNDO_PUSH) == 1);
	teardown();
}

/* ---- Multiline insertion ---- */

static void test_multiline_insert_flattens_buffer(void)
{
	int r;

	setup();
	for (r = 0; r < 64; r++) {
		editor_insert_row(bcur(), r, "a line of the buffer", 20);
	}
	editor_cursor_goto(2, 4);
	kg_perf_reset();
	editor_insert_text_raw("one\ntwo\n", 8);
	/* A local splice: inserting text with a newline
	 * used to serialise the whole buffer and rebuild every row from
	 * it; now only the split row and the rows it became are touched. */
	CHECK(counter(KG_PERF_BUFFER_FLATTEN) == 0);
	CHECK(counter(KG_PERF_BUFFER_REBUILD) == 0);
	CHECK(counter(KG_PERF_ROW_UPDATE) == 3);
	CHECK(bcur()->numrows == 64 + 2);
	CHECK(strncmp(bcur()->row[2].chars, "a lione", 7) == 0);
	CHECK(strcmp(bcur()->row[3].chars, "two") == 0);
	CHECK(strcmp(bcur()->row[4].chars, "ne of the buffer") == 0);
	CHECK(strcmp(bcur()->row[65].chars, "a line of the buffer") == 0);
	teardown();
}

/* ---- Edit-granular syntax notification ---- */

/* The property this slice exists for: one edit transaction is ONE syntax
 * notification, whatever it did to the buffer's topology.  Rendering is
 * still per row -- the rows really are rebuilt -- but the syntax layer is
 * told once, with a description of the whole edit, instead of being woken
 * per row while the row array is still being assembled
 * (doc/plans/kg-tree-sitter-plan.md, Phase 3).
 *
 * The shape, derived rather than observed:
 *   SYNTAX_EDIT  == 1   one syntax_after_edit() per successful replace.
 *   ROW_UPDATE   == 3   the new span is rows 2..4 -- the split row plus
 *                       the two the two '\n' opened -- and each is
 *                       rendered exactly once.
 *   SYNTAX_ROW   == 4   those three rows, plus one row below the span:
 *                       the propagation always re-examines the first row
 *                       under an edit, since the staged rows carry no
 *                       "before" hl_oc to compare against.  It stops
 *                       there because row 5's state comes out unchanged.
 *   PROPAGATE    == 1   that one row below the span, and no more.
 * Before this slice the same edit cost three notifications, each with its
 * own propagation, two of them over rows not yet rendered.
 *
 * SYNTAX_ROW and PROPAGATE are the BACKEND's half of that shape, and the
 * two backends genuinely disagree: the numbers above are the legacy
 * scanners', and the tree-sitter backend answers the notification with an
 * incremental parse against the old tree and a repaint of the damaged rows
 * (doc/plans/kg-tree-sitter-plan.md, Phase 7), which is a different, and
 * separately derived, small number.  Both are asserted.  SYNTAX_EDIT and
 * ROW_UPDATE are the facade's and are the same either way.
 *
 * The syntax_rebuild() before the edit is not decoration: it is the
 * notification a real buffer has already had by the time anyone types into
 * it -- a load prepares the parse, a mode change rebuilds it -- and
 * without it the first edit would be measuring a buffer acquiring its
 * parser rather than an ordinary keystroke. */
static void test_multiline_edit_notifies_syntax_once(void)
{
	const int rows = 64;
	struct kg_edit e;
	size_t pos;
	int r;

	setup();
	bcur()->syntax = syntax_find_by_name("C");
	for (r = 0; r < rows; r++) {
		editor_insert_row(bcur(), r, "int v = 0;", 10);
	}
	syntax_rebuild(bcur());
	pos = buffer_row_col_to_position(bcur(), 2, 4);
	e = kg_edit_user(bcur(), pos, pos, "one\ntwo\n", 8);
	kg_perf_reset();
	CHECK(kg_buffer_replace(&e, NULL) == 1);
	CHECK(bcur()->numrows == rows + 2);

	CHECK(counter(KG_PERF_SYNTAX_EDIT) == 1);
	CHECK(counter(KG_PERF_ROW_UPDATE) == 3);
#ifdef KG_USE_TREE_SITTER
	/* The tree-sitter backend's half of the shape, and the whole of
	 * Phase 7: ONE parse, and it is handed the old tree, so it is not a
	 * parse from nothing; the repaint is bounded by a constant that does
	 * not contain `rows`.
	 *
	 * The bound, derived rather than observed.  The damage set is the
	 * union of the edit's own new span -- rows 2..4, three rows, because
	 * the replacement carries two newlines -- and the rows of the
	 * changed ranges tree-sitter reports, which for text pasted inside
	 * one declaration is that same span (measured: one range, three rows
	 * repainted, on tree-sitter 0.26.11 with grammar c 0.24.2).  Five is
	 * that with a row of room at either end for a library or grammar
	 * bump to widen a range; what the number may never do is grow with
	 * the buffer, which is exactly what the slice-6 answer (rows + 2)
	 * did.  Every row the backend repaints is one the facade scans, so
	 * SYNTAX_ROW and TS_REHIGHLIGHT_ROW are the same number here, which
	 * is what says nothing else re-coloured anything behind their back. */
	CHECK(counter(KG_PERF_TS_PARSE) == 1);
	CHECK(counter(KG_PERF_TS_FULL_PARSE) == 0);
	CHECK(counter(KG_PERF_TS_CHANGED_RANGE) >= 1);
	CHECK(counter(KG_PERF_TS_REHIGHLIGHT_ROW) >= 3);
	CHECK(counter(KG_PERF_TS_REHIGHLIGHT_ROW) <= 5);
	CHECK(
	    counter(KG_PERF_SYNTAX_ROW) == counter(KG_PERF_TS_REHIGHLIGHT_ROW));
	CHECK(counter(KG_PERF_SYNTAX_PROPAGATE) == 0);
#else
	/* Three edited rows, the always-re-examined row below them, and the
	 * one row of look-behind above (the Markdown setext dependency):
	 * five, and still a constant. */
	CHECK(counter(KG_PERF_SYNTAX_ROW) == 5);
	CHECK(counter(KG_PERF_SYNTAX_PROPAGATE) == 1);
#endif
	teardown();
}

/* The same property for the hottest edit of all -- one typed byte, one
 * row, no topology change.  One notification, one rendered row, and -- for
 * the legacy scanners -- three rows scanned (the row itself, the
 * always-re-examined row below it, and the one row of look-behind above,
 * which is what keeps a setext heading honest), the scan stopping there
 * rather than walking the rest of the buffer.  The tree-sitter backend
 * reparses incrementally and repaints one row; see the note above. */
static void test_one_row_edit_notifies_syntax_once(void)
{
	const int rows = 64;
	int r;

	setup();
	bcur()->syntax = syntax_find_by_name("C");
	for (r = 0; r < rows; r++) {
		editor_insert_row(bcur(), r, "int v = 0;", 10);
	}
	syntax_rebuild(bcur());
	kg_perf_reset();
	CHECK(editor_row_replace_range(2, 4, 0, "y", 1, KG_EDIT_USER) == 1);

	CHECK(counter(KG_PERF_SYNTAX_EDIT) == 1);
	CHECK(counter(KG_PERF_ROW_UPDATE) == 1);
#ifdef KG_USE_TREE_SITTER
	/* The hottest edit there is, and the number this slice exists to
	 * make constant.  Derivation as above: the edit's new span is one
	 * row, and a byte typed into a declarator changes nothing outside
	 * the line it is on, so the changed range is inside that same row
	 * and the union is one row (measured: one range, one row).  Three is
	 * that with room for a range to reach a row further; 64 rows of
	 * buffer must not appear in it, and did in slice 6. */
	CHECK(counter(KG_PERF_TS_PARSE) == 1);
	CHECK(counter(KG_PERF_TS_FULL_PARSE) == 0);
	CHECK(counter(KG_PERF_TS_CHANGED_RANGE) >= 1);
	CHECK(counter(KG_PERF_TS_REHIGHLIGHT_ROW) >= 1);
	CHECK(counter(KG_PERF_TS_REHIGHLIGHT_ROW) <= 3);
	CHECK(
	    counter(KG_PERF_SYNTAX_ROW) == counter(KG_PERF_TS_REHIGHLIGHT_ROW));
	CHECK(counter(KG_PERF_SYNTAX_PROPAGATE) == 0);
#else
	CHECK(counter(KG_PERF_SYNTAX_ROW) == 3);
	CHECK(counter(KG_PERF_SYNTAX_PROPAGATE) == 1);
#endif
	teardown();
}

/* ---- Undo eviction ---- */

static void test_undo_eviction_walk(void)
{
	int i;
	const int max = 1000;

	setup();
	bcur()->undostack.max_size = max;
	editor_insert_row(bcur(), 0, "text", 4);
	kg_perf_reset();
	for (i = 0; i < max + 8; i++) {
		CHECK(undo_push_change(bcur(), 0, "x", 1, 1) == 1);
	}
	CHECK(bcur()->undostack.size == max);
	/* Trimming keeps max_size - 1 records, so a full stack evicts on
	 * every second push: 4 walks of 999 links for the 8 pushes past the
	 * limit.  This counter says whether replacing it with a deque is
	 * worthwhile; it is the number that says how much there is to win. */
	CHECK(counter(KG_PERF_UNDO_EVICT_LINKS)
	    == (unsigned long long)4 * (max - 1));
	teardown();
}

/* ---- Visual-line geometry ---- */

/* Build `rows` lines of visual-line-mode content of a caller-chosen width,
 * so one test can use short lines (one segment at the default 80-column
 * window) and another can pick a `len` a later resize shrinks the window
 * below, to force a wrap. */
static void setup_visual_line_rows(int rows, int len)
{
	char *line = malloc((size_t)len + 1);
	int i;

	CHECK(line != NULL);
	if (!line) {
		return;
	}
	memset(line, 'x', (size_t)len);
	line[len] = '\0';
	setup();
	for (i = 0; i < rows; i++) {
		editor_insert_row(bcur(), i, line, (size_t)len);
	}
	free(line);
	bcur()->visual_line_mode = 1;
}

/* Phase 1 acceptance: a warm, wholly unchanged repaint scans zero row
 * bytes.  Before the per-row wrapped-width cache, this was the opposite --
 * a second repaint cost exactly what the first one did, because nothing
 * remembered that a row's wrapped width at this window's width had
 * already been measured (test_visual_line_scan_per_refresh() below still
 * pins the cold case that made this worth fixing). */
static void test_visual_line_warm_repaint_scans_nothing(void)
{
	const int rows = 300;
	unsigned long long cold_scan;

	setup_visual_line_rows(rows, 40);

	kg_perf_reset();
	refresh_quietly();
	cold_scan = counter(KG_PERF_VISUAL_ROW_SCAN);
	CHECK(cold_scan > 0); /* the first repaint is still genuinely cold */

	kg_perf_reset();
	refresh_quietly();
	CHECK(counter(KG_PERF_VISUAL_ROW_SCAN) == 0);
	CHECK(counter(KG_PERF_VISUAL_BYTE_SCAN) == 0);
	/* Cursor motion alone -- no edit, no width change -- is the same
	 * warm case: get_visual_row() asks every row's width on the way to
	 * the cursor's row and none of them should scan either. */
	editor_move_cursor(ARROW_DOWN);
	editor_move_cursor(ARROW_DOWN);
	kg_perf_reset();
	refresh_quietly();
	CHECK(counter(KG_PERF_VISUAL_ROW_SCAN) == 0);
	CHECK(counter(KG_PERF_VISUAL_BYTE_SCAN) == 0);

	bcur()->visual_line_mode = 0;
	teardown();
}

/* Phase 1 acceptance: a one-row edit misses only that row's width cache,
 * at the window's current width -- every other row's cache survives the
 * edit untouched.  editor_update_row() invalidates unconditionally before
 * it can fail, so this also covers the row actually edited rather than a
 * neighbour. */
static void test_visual_line_edit_misses_only_that_row(void)
{
	const int rows = 300;

	setup_visual_line_rows(rows, 40);
	refresh_quietly(); /* warm every row's cache at the current width */

	kg_perf_reset();
	editor_row_insert_char(&bcur()->row[5], 0, 'y');
	refresh_quietly();
	CHECK(counter(KG_PERF_VISUAL_ROW_SCAN) == 1);
	CHECK(counter(KG_PERF_VISUAL_BYTE_SCAN)
	    == (unsigned long long)bcur()->row[5].size);

	bcur()->visual_line_mode = 0;
	teardown();
}

/* Phase 1 acceptance: a genuinely new window width performs at most one
 * cold byte scan per row -- the cache key is the width, so every row
 * misses exactly once at the new width and any later visit to the same
 * row within the same repaint (find_visual_row() restarting from row
 * zero per screen row) is warm again immediately. */
static void test_visual_line_new_width_scans_each_row_once(void)
{
	const int rows = 300;

	setup_visual_line_rows(rows, 40);
	refresh_quietly(); /* warm every row's cache at the old width */

	kg_perf_reset();
	wcur()->w = 20; /* narrower: 40-column lines now wrap in two */
	refresh_quietly();
	CHECK(counter(KG_PERF_VISUAL_ROW_SCAN) > 0);
	CHECK(counter(KG_PERF_VISUAL_ROW_SCAN) <= (unsigned long long)rows);

	bcur()->visual_line_mode = 0;
	teardown();
}

/* The row cache's RSS cost, recorded rather than left to be inferred:
 * window width, tab width and measured width are three int fields.  On a
 * 64-bit build they fill the struct through byte 64, so adding tab width
 * to the cache key consumes padding and does not enlarge erow. */
static void test_erow_wrap_cache_size_cost(void)
{
	erow r;

	CHECK(sizeof(r.wrap_cache_key) == 2 * sizeof(int));
	CHECK(sizeof(r.wrap_cache_vcols) == sizeof(int));
	CHECK(sizeof(erow) <= 64);
}

/* This assertion used to pin the bad shape on purpose (`> win_h`, "the
 * shape a later persistent prefix index has to fix").  Sub-plan 07-B
 * (doc/plans/2026-07-31-follow-ups/07-subplans/07b-one-traversal-draw.md,
 * "The assertion that must be inverted") is where that comes due, and
 * that document is the authorization for turning the `>` below into a
 * bound in the other direction -- a reviewer should be able to find this
 * sentence and that one together.
 *
 * It measured a *cold* repaint, which after sub-plan A no longer means
 * what its old name said: A routes find_visual_row() through the index,
 * so a cold call chain visits every row exactly once building the index
 * (one visual_segments() call per row, from vgeom_rebuild()), not
 * `win_h` times over.  That is a real cost, just not the one this test
 * was written to catch, so the warm-up repaint below is deliberately
 * excluded from the counters checked: it is what builds the index, and
 * this test is about what a *second*, unchanged repaint costs once the
 * index is warm.
 *
 * draw_window_rows() now places one vgeom_iter at the window's top
 * visual row -- one lower-bound binary search over the index's prefix
 * sums, no visual_segments() call at all -- and vgeom_iter_next() reads
 * each row's segment count straight out of those prefix sums as it
 * advances, rather than asking visual_segments() again per screen row.
 * So a warm repaint's prefix visits are not just bounded by
 * O(log rows + win_h), the shape sub-plan B named as the target: they
 * are exactly zero, strictly inside that bound. */
static void test_visual_line_prefix_visits_bounded_after_warmup(void)
{
	const int rows = 300;

	setup_visual_line_rows(rows, 10); /* narrower than win_w: one segment */
	refresh_quietly(); /* cold: builds the index, not under test here */

	kg_perf_reset();
	refresh_quietly(); /* warm repaint: what sub-plan B is judged on */
	CHECK(counter(KG_PERF_VISUAL_PREFIX_VISIT) == 0);

	bcur()->visual_line_mode = 0;
	teardown();
}

/* ---- Visual-line geometry index (src/vgeom.c), plan 07 phase 2 ---- */

/* Sub-plan 07a's shape acceptance: a query builds the index once
 * (REBUILD == 1, no HIT yet, since nothing existed to hit), and every
 * later query against the same key hits it -- no further row visited at
 * all, not even the O(log rows) some other design might still cost. */
static void test_vgeom_query_rebuilds_once_then_hits(void)
{
	const int rows = 300;

	setup_visual_line_rows(rows, 10);

	kg_perf_reset();
	CHECK(get_total_visual_rows(wcur(), bcur()) > 0);
	CHECK(counter(KG_PERF_VGEOM_REBUILD) == 1);
	CHECK(counter(KG_PERF_VGEOM_HIT) == 0);

	kg_perf_reset();
	(void)get_total_visual_rows(wcur(), bcur());
	(void)get_visual_row(wcur(), bcur(), 10, 0);
	(void)get_visual_row(wcur(), bcur(), 20, 0);
	CHECK(counter(KG_PERF_VGEOM_REBUILD) == 0);
	CHECK(counter(KG_PERF_VGEOM_HIT) == 3);

	bcur()->visual_line_mode = 0;
	teardown();
}

/* An edit bumps content_generation (src/buffer.c's buffer_note_change()),
 * which is part of the index's key, so the next query after an edit has
 * to rebuild -- but only once, not once per subsequent query. */
static void test_vgeom_edit_invalidates_and_rebuilds_once(void)
{
	const int rows = 300;

	setup_visual_line_rows(rows, 10);
	(void)get_total_visual_rows(wcur(), bcur());

	editor_row_insert_char(&bcur()->row[5], 0, 'x');

	kg_perf_reset();
	(void)get_total_visual_rows(wcur(), bcur());
	CHECK(counter(KG_PERF_VGEOM_REBUILD) == 1);

	kg_perf_reset();
	(void)get_total_visual_rows(wcur(), bcur());
	CHECK(counter(KG_PERF_VGEOM_REBUILD) == 0);
	CHECK(counter(KG_PERF_VGEOM_HIT) == 1);

	bcur()->visual_line_mode = 0;
	teardown();
}

/* The vsplit thrash sub-plan A's design exists to remove, as a counter
 * assertion rather than only a bench number: two windows on one buffer
 * at different widths (visual-line-vsplit-100k's 39/40 shape) each keep
 * their own index, so repeatedly querying both in turn costs one rebuild
 * per window -- ever -- not one per switch. */
static void test_vgeom_two_widths_do_not_evict_each_other(void)
{
	const int rows = 50;
	int i;

	setup_visual_line_rows(rows, 22);

	winlist[1] = winlist[0];
	winlist[1].vgeom = NULL;
	winlist[1].active = 1;
	winlist[1].w = winlist[0].w > 1 ? winlist[0].w - 1 : winlist[0].w;
	win_count = 2;

	(void)get_total_visual_rows(&winlist[0], bcur());
	(void)get_total_visual_rows(&winlist[1], bcur());

	kg_perf_reset();
	for (i = 0; i < 5; i++) {
		(void)get_total_visual_rows(&winlist[0], bcur());
		(void)get_total_visual_rows(&winlist[1], bcur());
	}
	CHECK(counter(KG_PERF_VGEOM_REBUILD) == 0);
	CHECK(counter(KG_PERF_VGEOM_HIT) == (unsigned long long)2 * 5);

	vgeom_window_free(&winlist[1]);
	winlist[1].active = 0;
	win_count = 1;
	bcur()->visual_line_mode = 0;
	teardown();
}

/* Historical aggregate case, kept for before/after continuity (see
 * doc/plans/2026-07-31-follow-ups/07-visual-line-geometry-index.md).
 * Before phase 1's cache, one repaint measured every row of the buffer
 * several times over -- find_visual_row() rescans from row 0 per screen
 * row, and the mode line's get_total_visual_rows() totals the whole
 * buffer again -- so `scans` was several multiples of `rows`.  The width
 * cache collapses every row revisited within one repaint to its first
 * (cold) visit, so even this still-cold first repaint now scans each row
 * at most once. */
static void test_visual_line_scan_per_refresh(void)
{
	const int rows = 500;
	int i;
	unsigned long long scans;

	setup();
	for (i = 0; i < rows; i++) {
		editor_insert_row(
		    bcur(), i, "a reasonably long line of text", 30);
	}
	bcur()->visual_line_mode = 1;
	kg_perf_reset();
	refresh_quietly();
	scans = counter(KG_PERF_VISUAL_ROW_SCAN);
	CHECK(scans > 0);
	CHECK(scans <= (unsigned long long)rows);
	bcur()->visual_line_mode = 0;
	teardown();
}

/* The decoration visible-range query's own cost shape: src/decor.c's
 * kg_decor_query_next() is a sorted-vector walk, called once per drawn
 * row (src/display.c), not once per repaint -- so EXAMINED grows with
 * (visible rows) x (decorations the walk has to pass before it can stop),
 * and VISIBLE is the exact number of decoration/row intersections a
 * repaint draws.  This is the measurement doc/plans/2026-07-31-follow-
 * ups/03-markers-decorations-and-events.md asks for before an interval
 * tree is ever considered, not a bound on it: there is no ceiling here to
 * violate, only a number to have before proposing a different structure.
 *
 * Buffer layout: 20 rows of "lineNN" (6 bytes) + 1 separator = 7 bytes of
 * flat position per row, rows 0-19 at positions 0, 7, 14, .... The window
 * shows only the first 5 (h=5), i.e. flat range [0,34) split into five
 * per-row queries at [0,6), [7,13), [14,20), [21,27), [28,34).
 *
 * Decoration A [0,4) intersects only row 0's range. Decoration B [10,14)
 * intersects only row 1's range [7,13) -- its own end sits exactly at row
 * 2's start, which the half-open test excludes. Decoration C [100,104) is
 * never inside any drawn row's range at all, and the sorted walk stops as
 * soon as it sees C's start past a row's range_end, so it is "examined"
 * only where the walk had not already stopped on B. */
static void test_decor_query_examines_and_returns_by_row(void)
{
	int i;
	char buf[16];

	setup();
	wcur()->h = 5;
	for (i = 0; i < 20; i++) {
		int len = snprintf(buf, sizeof(buf), "line%02d", i);

		editor_insert_row(bcur(), i, buf, len);
	}
	CHECK(kg_decor_create(bcur(), 0, 4, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);
	CHECK(kg_decor_create(bcur(), 10, 14, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false)
		  .id
	    != 0);
	CHECK(kg_decor_create(bcur(), 100, 104, KG_MARKER_GRAV_RIGHT,
		  KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 0, false)
		  .id
	    != 0);

	kg_perf_reset();
	refresh_quietly();

	CHECK(counter(KG_PERF_DECOR_VISIBLE) == 2);
	CHECK(counter(KG_PERF_DECOR_EXAMINED) == 14);
	CHECK(
	    counter(KG_PERF_DECOR_EXAMINED) >= counter(KG_PERF_DECOR_VISIBLE));

	teardown();
}

/* Phase 18's post-command-hook measurement, as the shape assertion it was
 * decided on.  The question the plan asks is whether an EMPTY
 * `post-command-hook' costs a keystroke anything; the answer is that the
 * seam does not reach fe at all -- RUNS stays 0 and not one arena slot
 * moves over a thousand keystrokes -- so what a session that never uses
 * the hook pays is one find_hook() string compare over a table that is
 * empty.  With one small hook installed every call runs, which is the
 * other half: the counter distinguishes the two cases rather than merely
 * being zero because nothing is wired up.
 *
 * A count, not a duration, for src/perf.h's own reason: this reads the
 * same under valgrind, in a sanitizer lane and on a loaded box. */
static void test_post_command_hook_empty_costs_nothing(void)
{
	struct kg_lisp_arena_stats before, after;
	char result[128] = "";
	int i;

	if (!kg_lisp_active()) {
		return;
	}
	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&before) == 0);
	for (i = 0; i < 1000; i++) {
		kg_lisp_run_post_command_hook();
	}
	CHECK(kg_perf_read(KG_PERF_POST_COMMAND_HOOK_CALLS) == 1000);
	CHECK(kg_perf_read(KG_PERF_POST_COMMAND_HOOK_RUNS) == 0);
	CHECK(kg_lisp_arena_stats(&after) == 0);
	CHECK(after.free_slots == before.free_slots);
	CHECK(after.collection_count == before.collection_count);

	CHECK(kg_lisp_eval_string("(progn (defvar pch-n 0)"
				  " (add-hook 'post-command-hook"
				  "  (lambda () (setq pch-n (1+ pch-n)))))",
		  sizeof("(progn (defvar pch-n 0)"
			 " (add-hook 'post-command-hook"
			 "  (lambda () (setq pch-n (1+ pch-n)))))")
		      - 1,
		  result, sizeof(result))
	    == 0);
	for (i = 0; i < 10; i++) {
		kg_lisp_run_post_command_hook();
	}
	CHECK(kg_perf_read(KG_PERF_POST_COMMAND_HOOK_CALLS) == 1010);
	CHECK(kg_perf_read(KG_PERF_POST_COMMAND_HOOK_RUNS) == 10);
	kg_lisp_shutdown();
}

/* A handful of forms shaped like an ordinary user init -- defvar, two
 * defuns (one documented, one interactive), a hook, two key bindings and
 * a small bounded recursion.  utils/bench.py's REPRESENTATIVE_INIT is the
 * same text, so the two baselines (this shape assertion and `make
 * bench`'s "lisp-arena-representative-init" case) describe one workload. */
static const char lisp_representative_init[]
    = "(defvar my-fill-column 100)"
      "(defun my-greet (name) \"Say hello.\" (message \"hi %s\" name))"
      "(defun my-count-words () (interactive) (message \"n/a\"))"
      /* A numeric comparison on purpose, post-Phase-2 (= is chained
       * numeric equality, not assignment) -- the return value is
       * deliberately discarded, since a hook body just needs to do a
       * trivial amount of work.  utils/bench.py keeps an equivalent
       * comment for the same reason. */
      "(defun my-before-save-hook () (= my-fill-column my-fill-column))"
      "(add-hook 'before-save-hook 'my-before-save-hook)"
      "(global-set-key \"C-c g\" \"my-greet\")"
      "(global-set-key \"C-c w\" \"my-count-words\")"
      "(defun my-loop (n acc) (if (<= n 0) acc (my-loop (- n 1) (cons n "
      "acc))))"
      "(my-loop 25 nil)";

/* Arena margin, not "does it fit": doc/plans/2026-08-03-elisp-subset-and-
 * fe-evaluator-subplans/00d-baselines-and-arena-observability.md asks how
 * much of the fixed 1 MiB arena remains free after the prelude, the
 * prelude plus lisp/auto-fill.el, and the prelude plus a representative
 * init -- before Phases 3-6 add frames, symbol cells and condition
 * objects to every allocation path.  Bounds here, not exact counts (this
 * file's convention): a durable margin claim survives an unrelated
 * prelude edit that shifts the exact object count by a few; a collapsed
 * one does not, and that is the failure this guards.  collection_count
 * staying exactly 1 is the one exact-count assertion -- the post-prelude
 * collect (doc/plans/2026-08-14-embedded-prelude.md, "Post-prelude
 * collect -- results") is kg_lisp_init()'s own last act, so it is already
 * counted by the time this function's first reading happens.  The
 * property this pins is that nothing AFTER it -- loading auto-fill.el, or
 * a representative init -- ever needs a collection of its own: "the
 * prelude and a representative init fit without ever forcing a SECOND
 * collection" is what survived from the original claim once the first
 * one stopped being optional. */
static void test_lisp_prelude_arena_margin(void)
{
	struct kg_lisp_arena_stats stats;

	if (!kg_lisp_active()) {
		return;
	}

	CHECK(kg_lisp_init() == 0);
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	CHECK(stats.total_slots > 0);
	CHECK(stats.allocation_failures == 0);
	CHECK(stats.collection_count == 1);
	/* The post-prelude collect did something: live objects right after
	 * kg_lisp_init() returns are strictly fewer than the high-water mark
	 * it took to get there -- a shape assertion rather than the exact
	 * 6819/5959 the prelude census manifest pins, so this survives an
	 * unrelated prelude edit that shifts the exact counts. */
	CHECK(stats.total_slots - stats.free_slots < stats.peak_live_objects);
	/* At least half the arena free after the prelude alone -- margin,
	 * not a tight fit. */
	CHECK(stats.free_slots * 2 > stats.total_slots);
	/* frame_capacity is a fixed property of the arena layout, not the
	 * workload; asserted nonzero and stable here (measured 1087 on this
	 * build) rather than to a specific number, per the same
	 * bound-not-count convention as the slot counts above. */
	CHECK(stats.frame_capacity > 0);
	CHECK(stats.peak_frame_depth <= stats.frame_capacity);
	/* Exactly 1, not merely nonzero: install_deferred_stubs() (src/
	 * lisp_prelude.c, Prelude Phase 1, b94e795 "defer the 93 names no
	 * startup path needs") runs right after the eager prelude and calls
	 * FeCallWithOptions() once PER DEFERRED NAME (93 times) to fetch
	 * `internal--make-deferred-stub` and build that name's stub closure
	 * -- a native re-entering the evaluator every time -- but never
	 * nested inside itself: each call returns before the next one
	 * starts, so the high-water mark of SIMULTANEOUS re-entry stays 1
	 * across all 93, the same as if there were only one.  peak_
	 * native_reentry is a depth, not a count, which is what makes that
	 * true; pinned so a future change to how deferred stubs are
	 * installed -- nesting one inside another, say -- says so loudly
	 * rather than the way this number drifted (0 -> 1) unremarked
	 * through the comment above that used to still say 0. */
	CHECK(stats.peak_native_reentry == 1);

	CHECK(kg_lisp_load_file("lisp/auto-fill.el") == 0);
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	/* Still 1, not 2: loading a package on top of the prelude needs no
	 * collection of its own. */
	CHECK(stats.collection_count == 1);
	CHECK(stats.free_slots * 2 > stats.total_slots);
	kg_lisp_shutdown();

	CHECK(kg_lisp_init() == 0);
	{
		char result[128] = "";

		CHECK(kg_lisp_eval_string(lisp_representative_init,
			  sizeof(lisp_representative_init) - 1, result,
			  sizeof(result))
		    == 0);
	}
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	/* Still 1, not 2: a representative init also needs no collection of
	 * its own. */
	CHECK(stats.collection_count == 1);
	CHECK(stats.free_slots * 2 > stats.total_slots);
	/* A real init evaluates real forms: peak_frame_depth has moved off
	 * the bare prelude's own baseline.  That baseline itself has moved
	 * since the Phase 12 fix cycle measured it at peak_frame_depth 3 --
	 * re-measured at this fe pin (Phase 21.2) via this same harness, the
	 * bare prelude alone now reads 8 (and peak_native_reentry 1, up from
	 * 0).  Explained, not just re-measured: Prelude Phase 1 (b94e795,
	 * "defer the 93 names no startup path needs") added
	 * install_deferred_stubs() (src/lisp_prelude.c), which
	 * kg_lisp_init() runs right after the eager prelude and which calls
	 * FeCallWithOptions() once per deferred name -- a NATIVE re-entering
	 * the evaluator to build that name's stub closure
	 * (`internal--make-deferred-stub`, lisp/prelude.el) and
	 * FeSetFunction() it in, which is the native_reentry 0 -> 1 and,
	 * via that closure's own construction nesting frames above the
	 * eager prelude, the frame_depth 3 -> 8 both at once.  See
	 * utils/bench.py's identical baseline comment for the fuller
	 * account; this file and that one describe the same prelude.  The
	 * representative init above reads 54, still well under
	 * frame_capacity.  The threshold below moved from 2 to 20 for
	 * exactly that reason: 2 is now BELOW
	 * the bare-prelude baseline of 8, so it would have passed for a
	 * kg_lisp_eval_string() call that silently did nothing at all --
	 * the same "case that cannot fail" defect utils/bench.py's
	 * lisp-arena-auto-fill and lisp-arithmetic-loop cases had at this
	 * same pin, for the same underlying reason (see their comments).
	 * 20 clears the current baseline of 8 with real margin, matching the
	 * threshold utils/bench.py's own lisp-arena-representative-init and
	 * lisp-arena-auto-fill cases use for the identical shape. */
	CHECK(stats.peak_frame_depth > 20);
	CHECK(stats.peak_cleanup_stack_depth == 0);
	kg_lisp_shutdown();
}

/* §15's three load-time counters, and the confusion the new two exist to
 * kill (sub-plan 10C Part 4, 10A Decision 9).
 *
 * The parent plan's §15 asks for prelude load time, user-init load time
 * and package load time before a bytecode project may be justified. Only
 * the first was instrumented, and the Phase 10 audit misread it: with an
 * init file present `lisp_prelude_ns` reports *less*, which invites the
 * conclusion that an init file makes the prelude faster. It does not --
 * the two are simply different work, and the prelude counter never
 * included any init time to begin with. The fix is not a bigger number
 * but two more counters, and this test is what pins the relationship
 * between them.
 *
 * What is asserted is *population and separation*, never a value: a wall
 * time is not the same number twice on a loaded box (this file's own
 * header), so "how long" is `make bench`'s and 10D's job to report, not
 * a gate's to assert.
 *
 *   - after kg_lisp_init() alone: prelude populated, init and package
 *     zero -- proof that the prelude counter is not silently the sum;
 *   - after an init file that requires a package: init populated and
 *     package populated, prelude UNCHANGED by either -- the separation;
 *   - package time is a running total, so a second require adds to it. */
static void test_lisp_load_time_counters(void)
{
	char root[64], path[512];
	unsigned long long prelude, init_ns, package;

	if (!kg_lisp_active()) {
		return;
	}
	CHECK(perf_config_root(root, sizeof(root)) == 0);

	(void)snprintf(path, sizeof(path), "%s/kg/lisp/perfpkg.el", root);
	CHECK(perf_write_file(path,
		  "(setq perf-pkg-loaded t)\n"
		  "(provide 'perfpkg)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/perfpkg2.el", root);
	CHECK(perf_write_file(path, "(provide 'perfpkg2)\n") == 0);
	/* A package that requires another: the shape that made the package
	 * total exceed the init total it happened inside, before only the
	 * outermost require of a chain was timed. */
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/perfouter.el", root);
	CHECK(perf_write_file(path,
		  "(require 'perfinner)\n"
		  "(provide 'perfouter)\n")
	    == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/lisp/perfinner.el", root);
	CHECK(perf_write_file(path, "(provide 'perfinner)\n") == 0);
	(void)snprintf(path, sizeof(path), "%s/kg/init.el", root);
	CHECK(perf_write_file(path,
		  "(require 'perfpkg)\n"
		  "(setq perf-init-ran t)\n")
	    == 0);

	kg_perf_reset();
	CHECK(kg_lisp_init() == 0);
	prelude = counter(KG_PERF_LISP_PRELUDE_NS);
	/* The prelude is real work and takes real time; zero here would mean
	 * the counter is not wired, which is the only thing worth asserting
	 * about a duration. */
	CHECK(prelude > 0);
	CHECK(counter(KG_PERF_LISP_USER_INIT_NS) == 0);
	CHECK(counter(KG_PERF_LISP_PACKAGE_LOAD_NS) == 0);

	CHECK(kg_lisp_load_init() == 0);
	init_ns = counter(KG_PERF_LISP_USER_INIT_NS);
	package = counter(KG_PERF_LISP_PACKAGE_LOAD_NS);
	CHECK(init_ns > 0);
	CHECK(package > 0);
	/* The separation: loading an init file did not touch the prelude
	 * counter in either direction.  This is the assertion the audit's
	 * "the prelude counter reports less with an init present" reading
	 * would have failed. */
	CHECK(counter(KG_PERF_LISP_PRELUDE_NS) == prelude);
	/* The nesting, stated rather than left to be rediscovered: the
	 * require is written in the init file, so its time is inside the
	 * init file's. */
	CHECK(package <= init_ns);
	/* The init file really ran, and really loaded the package: the
	 * counters above are timing work that happened. */
	{
		char result[128] = "";

		CHECK(
		    kg_lisp_eval_string("(list perf-init-ran perf-pkg-loaded)",
			sizeof("(list perf-init-ran perf-pkg-loaded)") - 1,
			result, sizeof(result))
		    == 0);
		CHECK(strcmp(result, "(t t)") == 0);
	}

	/* A total, not a gauge: a second require adds. */
	{
		static const char second[] = "(require 'perfpkg2)";

		CHECK(
		    kg_lisp_eval_string(second, sizeof(second) - 1, nullptr, 0)
		    == 0);
		CHECK(counter(KG_PERF_LISP_PACKAGE_LOAD_NS) > package);
		/* ... and it is not counted as init time, which has finished.
		 */
		CHECK(counter(KG_PERF_LISP_USER_INIT_NS) == init_ns);
		package = counter(KG_PERF_LISP_PACKAGE_LOAD_NS);
	}

	/* A nested require is counted ONCE, inside its outermost one.  A
	 * chain's inner interval lies entirely inside its parent's, so
	 * timing both counted the same nanoseconds twice -- measured on a
	 * real init file, where the package total came out LARGER than the
	 * init total it happened inside.  The assertion that catches a
	 * regression is not a sum (a wall time is never the same number
	 * twice) but a bound: the whole chain took at most the wall time
	 * this one eval took, which double counting exceeds by
	 * construction. */
	{
		static const char chain[] = "(require 'perfouter)";
		unsigned long long chain_ns;
		struct timespec before, after;

		clock_gettime(CLOCK_MONOTONIC, &before);
		CHECK(kg_lisp_eval_string(chain, sizeof(chain) - 1, nullptr, 0)
		    == 0);
		clock_gettime(CLOCK_MONOTONIC, &after);
		chain_ns = counter(KG_PERF_LISP_PACKAGE_LOAD_NS) - package;
		CHECK(chain_ns > 0);
		CHECK(chain_ns
		    <= (unsigned long long)((after.tv_sec - before.tv_sec)
			    * 1000000000LL
			+ (after.tv_nsec - before.tv_nsec)));
		/* Both features really were provided by that one chain. */
		{
			char result[128] = "";
			static const char both[] = "(list (featurep 'perfouter)"
						   " (featurep 'perfinner))";

			CHECK(kg_lisp_eval_string(both, sizeof(both) - 1,
				  result, sizeof(result))
			    == 0);
			CHECK(strcmp(result, "(t t)") == 0);
		}
	}

	kg_lisp_shutdown();
	perf_remove_config_root(root);
}

/* Fe evaluation throughput shapes (list walk, arithmetic loop,
 * macro-heavy expansion, deep call chain) -- the same four
 * utils/bench.py's Lisp cases exercise interactively, run in-process here
 * so a wrong result or a resource regression fails `make check` rather
 * than only showing up in a `make bench` a developer chose to run.
 *
 * Each asserts its own exact result: unlike a rebuild's incidental object
 * count, "this expression computed the right answer" is the property
 * under test, not an approximation of it.  Every shape here is also
 * bounded against GcStackSize (4096) and, where that is the resource the
 * shape actually spends, frame_capacity -- margin, not a tight number --
 * see the list-walk comment for the measurement that picked 150 and for
 * which of the two ceilings this shape actually meets. */
static void test_lisp_evaluator_shapes(void)
{
	static constexpr size_t gc_stack_size = 4096;
	char result[128];
	struct kg_lisp_arena_stats stats;

	if (!kg_lisp_active()) {
		return;
	}

	/* List walk: `lw` is not tail-call optimised (Fe's recursive
	 * evaluator does not flatten it), so every intermediate cons stays
	 * live until the outermost call returns.  This comment used to claim
	 * that made GC-stack depth linear in recursion depth and the
	 * resource this shape spends -- "peak_gc_stack_depth is 3914 of 4096
	 * at n=300 and overflows by n=400".  Re-measured at this fe pin
	 * (Phase 21.2), that claim is backwards: THE GC ROOT STACK DOES NOT
	 * GROW WITH n AT ALL.  peak_gc_stack_depth reads the same value --
	 * kg's own prelude baseline -- at n=150, n=300, n=400 and n=600
	 * alike (measured via utils/bench.py's counting build,
	 * test/perfobj/kg, one run per n).  What scales is peak_frame_depth:
	 * 305/605/805/1087 at those same four n, roughly 2 per recursion
	 * level (`lw`'s body is one `if` wrapping the recursive call, no
	 * extra arithmetic frame) -- exactly what fe's own Phase 21.2 commit
	 * found for the equivalent bare-context shape, confirmed here for
	 * kg's prelude-loaded evaluator too.  frame_capacity (1087 on this
	 * build) is the ceiling this shape actually meets: n=600 saturates
	 * it exactly and both this function's kg_lisp_eval_string() and
	 * test/kgbatch raise "evaluation frame limit exceeded" somewhere
	 * between n=520 (still fits) and n=540-545 (does not; the two entry
	 * paths' own frame overhead differs by a handful, hence the range).
	 * 150 leaves about 72% of frame_capacity free (305 of 1087) while
	 * still being a real multi-hundred-cell walk. */
	CHECK(kg_lisp_init() == 0);
	static const char list_walk[]
	    = "(defun lw (n l) (if (<= n 0) l (lw (- n 1) (cons n l)))) "
	      "(length (lw 150 nil))";
	result[0] = '\0';
	CHECK(kg_lisp_eval_string(
		  list_walk, sizeof(list_walk) - 1, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "150") == 0);
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	CHECK(stats.peak_gc_stack_depth < gc_stack_size);
	/* The bound this shape actually meets, per the comment above -- not
	 * asserted before this pin, which is exactly how a stale claim about
	 * the wrong resource went unnoticed. */
	CHECK(stats.peak_frame_depth < stats.frame_capacity);
	kg_lisp_shutdown();

	/* Arithmetic loop: 20000 `while` iterations of scalar addition --
	 * garbage per iteration (the `+`/`-` results), nothing retained, so
	 * this is the shape furthest from list-walk's GC-stack pressure. */
	CHECK(kg_lisp_init() == 0);
	static const char arithmetic_loop[]
	    = "(setq i 0) (setq acc 0) (while (< i 20000) (setq acc (+ acc i)) "
	      "(setq i (+ i 1))) acc";
	result[0] = '\0';
	CHECK(kg_lisp_eval_string(arithmetic_loop, sizeof(arithmetic_loop) - 1,
		  result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "199990000") == 0);
	kg_lisp_shutdown();

	/* Macro-heavy: fe.c:1863 (doc/fe-upstream.md) re-expands a macro on
	 * every call rather than overwriting the call site, so 2000 calls
	 * is 2000 expansions charged against the step budget, not one. */
	CHECK(kg_lisp_init() == 0);
	static const char macro_heavy[]
	    = "(defmacro m (x) (list '+ x 1)) (setq n 0) (setq i 0) "
	      "(while (< i 2000) (setq n (m n)) (setq i (+ i 1))) n";
	result[0] = '\0';
	CHECK(kg_lisp_eval_string(
		  macro_heavy, sizeof(macro_heavy) - 1, result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "2000") == 0);
	kg_lisp_shutdown();

	/* Deep call chain: 300 levels of non-tail self-recursion, well under
	 * both the GC-stack ceiling and the arena's own frame_capacity
	 * (measured peak_frame_depth 904 of frame_capacity 1087 on this
	 * build -- about 3 frames per recursion level for this chain's
	 * shape: the `if`, the `+`, and the recursive call each open one).
	 * Asserted against frame_capacity rather than a hardcoded number so
	 * this stays meaningful if KG_LISP_ARENA_SIZE, or Fe's per-frame
	 * arena partition, ever changes; the assertion is what
	 * test_recursion_depth's comment (test_lisp.c) also measures. */
	CHECK(kg_lisp_init() == 0);
	static const char deep_call_chain[]
	    = "(defun dc (n) (if (<= n 0) 0 (+ 1 (dc (- n 1))))) (dc 300)";
	result[0] = '\0';
	CHECK(kg_lisp_eval_string(deep_call_chain, sizeof(deep_call_chain) - 1,
		  result, sizeof(result))
	    == 0);
	CHECK(strcmp(result, "300") == 0);
	CHECK(kg_lisp_arena_stats(&stats) == 0);
	CHECK(stats.peak_gc_stack_depth < gc_stack_size);
	CHECK(stats.peak_frame_depth < stats.frame_capacity);
	kg_lisp_shutdown();
}

/* The integer value fe's own FePerfWriteJson() wrote after `"key": `,
 * found by the same kind of substring search a reader (or `utils/
 * bench.py`) does on this file's JSON -- there is no JSON parser in this
 * tree's C tests, and fe's flat "name": number shape does not need one.
 * Fails the test (does not return 0) if `key` is absent, since an absent
 * counter is a schema break this test exists to catch. */
static unsigned long long fe_json_counter(const char *json, const char *key)
{
	char needle[64];
	const char *p;

	CHECK(snprintf(needle, sizeof(needle), "\"%s\": ", key)
	    < (int)sizeof(needle));
	p = strstr(json, needle);
	CHECK(p != nullptr);
	if (p == nullptr) {
		return 0;
	}
	return strtoull(p + strlen(needle), nullptr, 10);
}

/* Phase 21's follow-up (doc/plans/2026-08-18-elisp-data-model-phase21-
 * baseline.md's "LIMITATION" paragraph): this counting build's fe
 * objects are compiled with FE_PERF_COUNTERS=1 too (Makefile's
 * PERF_FE_CFLAGS), so kg_lisp_perf_dump_fe_json() (src/lisp_core.c)
 * reaches fe's real FePerfWriteJson() rather than writing the disabled
 * build's JSON `null` -- checked directly below, not assumed, since
 * `null` would otherwise look like a passing string search on the wrong
 * build. Asserts relationships, per this file's own header rule and
 * fe's own Phase 21.1 commit's rule for its C API test: a cons cell
 * moves alloc_pair, and the by-type block still sums to alloc_object
 * from kg's side of the seam, the same invariant fe's own test_api.c
 * pins from fe's side. */
static void test_fe_perf_counters_reach_kg_json(void)
{
	FILE *fp;
	char buf[8192];
	size_t n;
	unsigned long long pairs_before, pairs_after, object_total;
	int i;

	if (!kg_lisp_active()) {
		return;
	}
	CHECK(kg_lisp_init() == 0);

	fp = tmpfile();
	CHECK(fp != nullptr);
	kg_lisp_perf_dump_fe_json(fp);
	rewind(fp);
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	CHECK(strncmp(buf, "null", 4) != 0);
	pairs_before = fe_json_counter(buf, "alloc_pair");

	{
		char result[128] = "";
		static const char one_cons[] = "(cons 1 2)";

		CHECK(kg_lisp_eval_string(one_cons, sizeof(one_cons) - 1,
			  result, sizeof(result))
		    == 0);
	}

	fp = tmpfile();
	CHECK(fp != nullptr);
	kg_lisp_perf_dump_fe_json(fp);
	rewind(fp);
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	pairs_after = fe_json_counter(buf, "alloc_pair");
	CHECK(pairs_after > pairs_before);

	object_total = 0;
	static const char *const alloc_type_names[] = {
		"alloc_pair",
		"alloc_free",
		"alloc_nil",
		"alloc_double",
		"alloc_integer",
		"alloc_symbol",
		"alloc_string",
		"alloc_fn",
		"alloc_macro",
		"alloc_primitive",
		"alloc_native_fn",
		"alloc_ptr",
		"alloc_fex0",
		"alloc_fex1",
		"alloc_fex2",
		"alloc_sentinel",
	};
	for (i = 0;
	    i < (int)(sizeof(alloc_type_names) / sizeof(alloc_type_names[0]));
	    i++) {
		object_total += fe_json_counter(buf, alloc_type_names[i]);
	}
	CHECK(object_total == fe_json_counter(buf, "alloc_object"));
	CHECK(fe_json_counter(buf, "peak_live_objects") > 0);
	kg_lisp_shutdown();
}

int main(void)
{
	RUN(test_post_command_hook_empty_costs_nothing);
	RUN(test_lisp_prelude_arena_margin);
	RUN(test_lisp_load_time_counters);
	RUN(test_lisp_evaluator_shapes);
	RUN(test_fe_perf_counters_reach_kg_json);
	RUN(test_load_row_array_growth);
	RUN(test_load_highlight_is_final);
	RUN(test_insert_row_array_growth);
	RUN(test_special_text_append_growth);
	RUN(test_compilation_mirror_updates_per_read);
#ifdef KG_USE_LSP
	RUN(test_batched_frames_cost_one_read);
#endif
	RUN(test_idle_wait_ends_on_a_ready_descriptor);
	RUN(test_idle_wait_ends_on_its_tick);
	RUN(test_pre_announce_pipe_loads_cost_wakes_not_ticks);
	RUN(test_idle_wait_prefers_the_terminal);
	RUN(test_frame_append_growth);
	RUN(test_long_row_update_allocations);
	RUN(test_typing_into_long_row_reuses_storage);
	RUN(test_one_byte_edit_in_long_row_derived_storage);
	RUN(test_replace_range_updates_once);
	RUN(test_rect_delete_updates_per_byte);
	RUN(test_multiline_insert_flattens_buffer);
	RUN(test_multiline_edit_notifies_syntax_once);
	RUN(test_one_row_edit_notifies_syntax_once);
	RUN(test_undo_eviction_walk);
	RUN(test_visual_line_scan_per_refresh);
	RUN(test_visual_line_warm_repaint_scans_nothing);
	RUN(test_visual_line_edit_misses_only_that_row);
	RUN(test_visual_line_new_width_scans_each_row_once);
	RUN(test_visual_line_prefix_visits_bounded_after_warmup);
	RUN(test_vgeom_query_rebuilds_once_then_hits);
	RUN(test_vgeom_edit_invalidates_and_rebuilds_once);
	RUN(test_vgeom_two_widths_do_not_evict_each_other);
	RUN(test_erow_wrap_cache_size_cost);
	RUN(test_decor_query_examines_and_returns_by_row);
	return test_summary();
}
