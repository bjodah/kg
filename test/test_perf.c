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
#include "../src/syntax.h"
#include "../src/vgeom.h"
#include "test.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
 * same hl bytes, same hl_oc, row for row. */
static void check_load_highlight_is_final(
    const char *name_template, int suffix_len, void (*write_corpus)(FILE *))
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
	    "test_perf_hl_XXXXXX.c", 2, write_comment_c);
	check_load_highlight_is_final(
	    "test_perf_hl_XXXXXX.md", 3, write_markdown);
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

/* Phase 1's RSS cost, recorded rather than left to be inferred: erow
 * (def.h) gained wrap_cache_win_w and wrap_cache_vcols, two int fields.
 * On a 64-bit build that lands exactly in the struct's existing trailing
 * padding after hl_oc (56 bytes rounded up to 64 for the pointers'
 * 8-byte alignment either way), so the cache costs 8 bytes/row here and
 * adds no padding of its own; a build where int and pointer alignment
 * differ could see up to 16. */
static void test_erow_wrap_cache_size_cost(void)
{
	erow r;

	CHECK(sizeof(r.wrap_cache_win_w) == sizeof(int));
	CHECK(sizeof(r.wrap_cache_vcols) == sizeof(int));
	CHECK(sizeof(erow) <= 64);
}

/* The prefix-summing walk's own shape, independent of whether a visit
 * rescans bytes: one cold repaint over `rows` lines, each fitting one
 * segment, visits more rows than the buffer has -- find_visual_row()
 * restarts from row zero for every one of the window's screen rows, so
 * row zero alone is visited once per screen row.  This is the shape a
 * later persistent prefix index (plan 07 phase 2) has to fix; phase 1's
 * width cache does not by itself change it. */
static void test_visual_line_prefix_walk_restarts_per_screen_row(void)
{
	const int rows = 300;

	setup_visual_line_rows(rows, 10); /* narrower than win_w: one segment */

	kg_perf_reset();
	refresh_quietly();
	CHECK(counter(KG_PERF_VISUAL_PREFIX_VISIT)
	    > (unsigned long long)wcur()->h);

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

int main(void)
{
	RUN(test_load_row_array_growth);
	RUN(test_load_highlight_is_final);
	RUN(test_insert_row_array_growth);
	RUN(test_special_text_append_growth);
	RUN(test_compilation_mirror_updates_per_read);
	RUN(test_frame_append_growth);
	RUN(test_long_row_update_allocations);
	RUN(test_typing_into_long_row_reuses_storage);
	RUN(test_one_byte_edit_in_long_row_derived_storage);
	RUN(test_replace_range_updates_once);
	RUN(test_rect_delete_updates_per_byte);
	RUN(test_multiline_insert_flattens_buffer);
	RUN(test_undo_eviction_walk);
	RUN(test_visual_line_scan_per_refresh);
	RUN(test_visual_line_warm_repaint_scans_nothing);
	RUN(test_visual_line_edit_misses_only_that_row);
	RUN(test_visual_line_new_width_scans_each_row_once);
	RUN(test_visual_line_prefix_walk_restarts_per_screen_row);
	RUN(test_vgeom_query_rebuilds_once_then_hits);
	RUN(test_vgeom_edit_invalidates_and_rebuilds_once);
	RUN(test_vgeom_two_widths_do_not_evict_each_other);
	RUN(test_erow_wrap_cache_size_cost);
	RUN(test_decor_query_examines_and_returns_by_row);
	return test_summary();
}
