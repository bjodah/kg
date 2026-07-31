/* test_winmgr.c — characterization of buffer, window and view ownership.
 *
 * Every other unit test binary stubs the window layer away.  This one links
 * the real winmgr.o against the real bufmgr.o so the questions this suite
 * exists to answer can be asked at all: which record owns a field, which
 * pointer each slot holds, and what crosses over when the user switches a
 * buffer or a window.
 *
 * The assertions are deliberately about pointers, slots and counters rather
 * than saved bytes: an outcome-only test passes straight through an ownership
 * bug, which is the class of bug this file is the net for.
 *
 * `bslot()`/`wslot()` below are the whole reason this reads the way it does.
 * While the live state of the *current* buffer sits in `editor` and is only
 * copied into `buflist[buf_current]` by the save protocol, a test that wants
 * the authoritative record has to run that protocol first.  When the copy
 * protocols are gone the helpers collapse to a plain `&buflist[i]`.
 */

#include "../src/def.h"
#include "test.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- Helpers ---- */

static char tmpdir[128];

static void write_text_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f) {
		return;
	}
	fputs(text, f);
	fclose(f);
}

static const char *tmppath(const char *name)
{
	static char buf[4][256];
	static int turn;

	turn = (turn + 1) % 4;
	snprintf(buf[turn], sizeof(buf[turn]), "%s/%s", tmpdir, name);
	return buf[turn];
}

/* The authoritative record for buffer slot `i`. */
static struct editor_buffer *bslot(int i)
{
	if (i == buf_current) {
		buf_save_current_state();
	}
	return &buflist[i];
}

/* The authoritative record for window slot `i`. */
static struct editor_window *wslot(int i)
{
	if (i == win_current) {
		win_save_active_view();
	}
	return &winlist[i];
}

/* A fresh session with one window and the named files open, current buffer 0.
 */
static void session(int nfiles, char **names)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		buflist[i].active = 0;
	}
	memset(&editor, 0, sizeof(editor));
	editor.readonly_override = -1;
	undo_stack_init(&bcur()->undostack);
	win_total_rows = 24;
	win_total_cols = 80;
	buf_load_args(nfiles, names, 0);
	win_init();
	win_restore_active_view();
}

/* editor_cleanup() runs at most once per process, so this suite frees its
 * own sessions.  Every slot owns its rows, its filename and its undo chain,
 * which is exactly the list a leak here would prove wrong. */
static void session_teardown(void)
{
	int i, j;

	buf_save_current_state();
	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		struct undo_op *op;

		if (!b->active) {
			continue;
		}
		for (j = 0; j < b->numrows; j++) {
			editor_free_row(&b->row[j]);
		}
		free(b->row);
		free(b->filename);
		for (op = b->undostack.head; op;) {
			struct undo_op *next = op->next;
			free(op->text);
			free(op);
			op = next;
		}
	}
	memset(buflist, 0, sizeof(buflist));
	buf_count = 0;
	buf_current = 0;
	memset(&editor, 0, sizeof(editor));
	bcur()->row = NULL;
	bcur()->numrows = 0;
	bcur()->row_capacity = 0;
	undo_stack_init(&bcur()->undostack);
	win_count = 0;
	win_current = 0;
	memset(winlist, 0, sizeof(winlist));
}

/* Index of the one active window that is not `win_current`. */
static int other_window(void)
{
	int i;

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active && i != win_current) {
			return i;
		}
	}
	return -1;
}

static int undo_depth(const struct editor_buffer *b)
{
	return b->undostack.size;
}

/* ---- Tests ---- */

/* Two open files are two slots, each holding its own row array and its own
 * filename.  Nothing that belongs to a buffer may be reachable from the
 * other slot. */
static void test_two_buffers_own_disjoint_text(void)
{
	char *names[2];

	write_text_file(tmppath("a.txt"), "alpha\nbeta\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\nthree\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);

	CHECK(buf_count == 2);
	CHECK(buflist[0].active && buflist[1].active);
	CHECK(bslot(0)->row != NULL);
	CHECK(bslot(1)->row != NULL);
	CHECK(bslot(0)->row != bslot(1)->row);
	CHECK(bslot(0)->filename != bslot(1)->filename);
	/* A file that ends in a newline opens a final empty row, so two
	 * text lines are three rows. */
	CHECK(bslot(0)->numrows == 3);
	CHECK(bslot(1)->numrows == 4);
	CHECK(bslot(0)->undostack.head == NULL);
	CHECK(bslot(1)->undostack.head == NULL);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* Edit, mark, read-only and compile-command are per buffer.  A round trip
 * through the other buffer must not carry any of them across, and must not
 * carry the other buffer's values back. */
static void test_switch_round_trip_keeps_fields_in_their_buffer(void)
{
	char *names[2];
	erow *rows0;

	write_text_file(tmppath("a.txt"), "alpha\nbeta\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\nthree\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);

	/* Buffer 0: dirty it, set a mark, make it read-only, give it a
	 * compile command. */
	editor.cy = 1;
	editor.cx = 2;
	editor_insert_char('X');
	editor.mark_set = 1;
	editor.mark_row = 0;
	editor.mark_col = 3;
	editor_set_local_readonly(1);
	snprintf(editor.compile_command, sizeof(editor.compile_command),
	    "make zero");
	editor.compile_command_user_override = 1;
	rows0 = bslot(0)->row;

	CHECK(bslot(0)->dirty != 0);
	CHECK(undo_depth(bslot(0)) == 1);

	/* Switch to buffer 1. */
	buf_save_current_state();
	buf_restore_from_slot(1);

	CHECK(buf_current == 1);
	CHECK(bcur()->dirty == 0);
	CHECK(editor.mark_set == 0);
	CHECK(editor.readonly == 0);
	CHECK(strcmp(editor.compile_command, "make -k") == 0);
	CHECK(editor.compile_command_user_override == 0);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->row != rows0);

	/* Buffer 0's record kept everything. */
	CHECK(buflist[0].dirty != 0);
	CHECK(buflist[0].mark_set == 1);
	CHECK(buflist[0].mark_col == 3);
	CHECK(buflist[0].readonly_local == 1);
	CHECK(strcmp(buflist[0].compile_command, "make zero") == 0);
	CHECK(buflist[0].undostack.size == 1);
	CHECK(buflist[0].row == rows0);

	/* Edit buffer 1 so it has its own history, then switch back. */
	editor_insert_char('Y');
	buf_save_current_state();
	buf_restore_from_slot(0);

	CHECK(buf_current == 0);
	CHECK(bcur()->row == rows0);
	CHECK(bcur()->dirty != 0);
	CHECK(editor.mark_set == 1 && editor.mark_col == 3);
	CHECK(editor.readonly == 1);
	CHECK(strcmp(editor.compile_command, "make zero") == 0);
	CHECK(bcur()->undostack.size == 1);
	CHECK(buflist[1].undostack.size == 1);
	CHECK(buflist[1].dirty != 0);
	CHECK(buflist[1].mark_set == 0);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* C-x 2 puts the same buffer in two windows.  The text is shared; the point
 * is not. */
static void test_split_shares_text_not_point(void)
{
	char *names[1];
	int other;

	write_text_file(tmppath("a.txt"), "l0\nl1\nl2\nl3\nl4\nl5\nl6\nl7\n");
	names[0] = strdup(tmppath("a.txt"));

	session(1, names);
	CHECK(win_count == 1);

	win_split_horizontal();
	CHECK(win_count == 2);
	other = other_window();
	CHECK(other >= 0);
	CHECK(winlist[win_current].bufidx == winlist[other].bufidx);

	/* Move point in the selected window only. */
	editor.cy = 2;
	editor.cx = 1;
	CHECK(wslot(win_current)->cy == 2);
	CHECK(winlist[other].cy == 0);

	/* Now select the other window and confirm it kept its own point. */
	win_cycle_next();
	CHECK(win_current == other);
	CHECK(editor.cy == 0);
	CHECK(editor.cx == 0);
	/* Same buffer, same rows. */
	CHECK(bcur()->row == buflist[winlist[win_current].bufidx].row);

	/* An edit through either window reaches the one shared text. */
	editor_insert_char('Z');
	CHECK(bslot(0)->numrows == 9);
	CHECK(bslot(0)->row[0].chars[0] == 'Z');

	session_teardown();
	free(names[0]);
}

/* C-x 3 is the same story in a new column group, and reflow gives the two
 * windows disjoint column bands. */
static void test_vertical_split_geometry(void)
{
	char *names[1];
	int other;

	write_text_file(tmppath("a.txt"), "l0\nl1\nl2\nl3\n");
	names[0] = strdup(tmppath("a.txt"));

	session(1, names);
	win_split_vertical();
	CHECK(win_count == 2);
	other = other_window();
	CHECK(other >= 0);
	CHECK(winlist[other].col_group != winlist[win_current].col_group);
	/* 80 columns, one separator column, two groups. */
	CHECK(winlist[win_current].w + winlist[other].w == 79);
	CHECK(winlist[other].x > winlist[win_current].x);
	CHECK(winlist[other].h == winlist[win_current].h);
	CHECK(editor.screencols == winlist[win_current].w);
	CHECK(editor.screenrows == winlist[win_current].h);

	session_teardown();
	free(names[0]);
}

/* Showing another buffer in the other window retargets that window without
 * disturbing the selected one. */
static void test_display_other_window_retargets_only_that_window(void)
{
	char *names[2];
	int other;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	win_display_buffer_other_window(1);
	CHECK(win_count == 2);
	other = other_window();
	CHECK(other >= 0);
	CHECK(winlist[other].bufidx == 1);
	CHECK(winlist[win_current].bufidx == 0);
	CHECK(buf_current == 0);
	CHECK(bcur()->row == bslot(0)->row);

	/* Selecting the other window follows its buffer. */
	win_cycle_next();
	CHECK(win_current == other);
	CHECK(buf_current == 1);
	CHECK(bcur()->numrows == 3);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* C-x 0 drops the selected window and selects a survivor; C-x 1 drops the
 * rest.  Neither may lose the buffer the survivor was showing. */
static void test_delete_window_paths(void)
{
	char *names[2];

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	win_display_buffer_other_window(1);
	CHECK(win_count == 2);

	win_delete_current(); /* C-x 0 */
	CHECK(win_count == 1);
	CHECK(winlist[win_current].active);
	CHECK(winlist[win_current].bufidx == 1);
	CHECK(buf_current == 1);
	CHECK(editor.screenrows == winlist[win_current].h);

	win_split_horizontal();
	CHECK(win_count == 2);
	win_delete_others(); /* C-x 1 */
	CHECK(win_count == 1);
	CHECK(winlist[win_current].active);
	CHECK(buf_current == 1);
	CHECK(editor.screenrows == winlist[win_current].h);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* Killing a buffer that two windows show leaves both windows pointing at a
 * dead slot today.  Characterized, not fixed. */
static void test_kill_buffer_shown_twice(void)
{
	char *names[2];
	int other;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);
	CHECK(winlist[other].bufidx == 0);

	buf_kill(-1);
	CHECK(buf_count == 1);
	CHECK(!buflist[0].active);
	CHECK(buflist[0].row == NULL);
	CHECK(buflist[0].row_capacity == 0);
	CHECK(buflist[0].filename == NULL);
	CHECK(buf_current == 1);
	CHECK(winlist[win_current].bufidx == 1);
	/* The unselected window still names the killed slot: a bare index
	 * that outlived the buffer it referred to. */
	CHECK(winlist[other].bufidx == 0);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* The buffer table is a fixed set of slots.  A killed slot is reused by the
 * next open, and nothing of the old buffer may survive into it. */
static void test_slot_exhaustion_and_reuse(void)
{
	char *names[1];
	char name[64];
	int i, reused;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	for (i = 1; i < MAX_BUFFERS; i++) {
		snprintf(name, sizeof(name), "f%02d.txt", i);
		write_text_file(tmppath(name), "x\n");
		buf_open_path(tmppath(name), 0);
	}
	CHECK(buf_count == MAX_BUFFERS);

	/* One more is refused rather than overwriting a slot. */
	write_text_file(tmppath("overflow.txt"), "x\n");
	buf_open_path(tmppath("overflow.txt"), 0);
	CHECK(buf_count == MAX_BUFFERS);

	/* Kill slot 5's buffer and reopen: the freed slot is taken. */
	buf_save_current_state();
	buf_restore_from_slot(5);
	CHECK(bslot(5)->row != NULL);
	CHECK(bslot(5)->filename != NULL);
	buf_kill(-1);
	CHECK(buf_count == MAX_BUFFERS - 1);
	CHECK(!buflist[5].active);

	buf_open_path(tmppath("overflow.txt"), 0);
	CHECK(buf_count == MAX_BUFFERS);
	reused = -1;
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (buflist[i].active && buflist[i].filename
		    && strstr(buflist[i].filename, "overflow.txt")) {
			reused = i;
		}
	}
	CHECK(reused == 5);
	/* The reused slot carries nothing of the buffer that had it. */
	CHECK(strstr(bslot(5)->filename, "overflow.txt") != NULL);
	CHECK(bslot(5)->undostack.head == NULL);
	CHECK(bslot(5)->dirty == 0);
	CHECK(bslot(5)->mark_set == 0);

	session_teardown();
	free(names[0]);
}

/* A background producer appends to a buffer nobody is looking at.  The
 * current buffer must not notice: same rows, same filename, same undo
 * stack, same point. */
static void test_append_to_hidden_buffer_leaves_current_alone(void)
{
	char *names[1];
	int target;
	erow *rows0;
	char *filename0;

	write_text_file(tmppath("a.txt"), "alpha\nbeta\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	editor.cy = 1;
	editor_insert_char('Q');
	rows0 = bcur()->row;
	filename0 = editor.filename;

	target
	    = buf_prepare_special_text("*background*", &compilation_syntax, 1);
	CHECK(target > 0);
	CHECK(target != buf_current);
	CHECK(buf_append_special_text(target, "line one\nline two\n", 18) == 0);

	CHECK(buf_current == 0);
	CHECK(bcur()->row == rows0);
	CHECK(editor.filename == filename0);
	CHECK(bcur()->numrows == 3);
	CHECK(editor.cy == 1);
	CHECK(bcur()->dirty != 0);
	CHECK(bcur()->undostack.size == 1);

	CHECK(buflist[target].numrows == 3);
	CHECK(buflist[target].row != rows0);
	CHECK(buflist[target].dirty == 0);
	CHECK(buflist[target].undostack.head == NULL);
	CHECK(buflist[target].syntax == &compilation_syntax);
	CHECK(memcmp(buflist[target].row[0].chars, "line one", 8) == 0);

	buf_clear_special_text(target);
	CHECK(buflist[target].numrows == 0);
	CHECK(bcur()->row == rows0);
	CHECK(bcur()->numrows == 3);

	session_teardown();
	free(names[0]);
}

/* Same producer, but the target buffer is on screen in the other window.
 * That window follows the tail; the selected window does not move. */
static void test_append_to_visible_buffer_follows_that_window(void)
{
	char *names[1];
	int target, other;

	write_text_file(tmppath("a.txt"), "alpha\nbeta\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	target
	    = buf_prepare_special_text("*background*", &compilation_syntax, 1);
	CHECK(target > 0);
	win_display_buffer_other_window(target);
	other = other_window();
	CHECK(other >= 0);
	CHECK(winlist[other].bufidx == target);

	editor.cy = 1;
	CHECK(buf_append_special_text(target, "one\ntwo\nthree\n", 14) == 0);

	CHECK(buf_current == 0);
	CHECK(editor.cy == 1);
	CHECK(buflist[target].numrows == 4);
	/* The unselected window tracked the tail it was already at. */
	CHECK(winlist[other].rowoff + winlist[other].cy
	    == buflist[target].numrows - 1);

	session_teardown();
	free(names[0]);
}

/* Reflow is geometry only.  It must hand out non-negative heights and at
 * least one column at every terminal size the editor accepts, and it must
 * leave the echo-area row free. */
static void test_reflow_sizes(void)
{
	char *names[1];
	int i;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	CHECK(winlist[0].h == 22); /* 24 rows - mode line - echo area */
	CHECK(winlist[0].w == 80);

	win_total_rows = 10;
	win_total_cols = 40;
	win_reflow();
	CHECK(winlist[0].h == 8);
	CHECK(winlist[0].w == 40);
	CHECK(editor.screenrows == 8);
	CHECK(editor.screencols == 40);

	/* A window too small to split says so and stays one window. */
	win_total_rows = 4;
	win_reflow();
	CHECK(winlist[0].h == 2);
	win_split_horizontal();
	CHECK(win_count == 1);

	/* Four horizontal splits at 24 rows still give every window a
	 * non-negative height and the echo area its row. */
	win_total_rows = 24;
	win_total_cols = 80;
	win_reflow();
	for (i = 0; i < 3; i++) {
		win_split_horizontal();
	}
	CHECK(win_count == 4);
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		CHECK(winlist[i].h >= 0);
		CHECK(winlist[i].w >= 1);
		CHECK(winlist[i].y + winlist[i].h <= win_total_rows);
	}

	session_teardown();
	free(names[0]);
}

/* A handle taken before a kill must not resolve afterwards, and must not
 * resolve to whatever buffer is handed the slot next. */
static void test_handles_do_not_survive_their_buffer(void)
{
	char *names[2];
	struct kg_buffer_handle killed, live;
	int i;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);

	killed = buf_handle(0);
	live = buf_handle(1);
	CHECK(killed.id != 0);
	CHECK(live.id != 0);
	CHECK(killed.id != live.id);
	CHECK(buf_resolve(killed) == &buflist[0]);
	CHECK(buf_handle_slot(live) == 1);

	buf_kill(-1); /* kills buffer 0, the current one */
	CHECK(buf_resolve(killed) == NULL);
	CHECK(buf_handle_slot(killed) == -1);
	CHECK(buf_resolve(live) == &buflist[1]);

	/* Slot 0 comes back as somebody else's. */
	write_text_file(tmppath("c.txt"), "gamma\n");
	buf_open_path(tmppath("c.txt"), 0);
	CHECK(buflist[0].active);
	CHECK(buf_resolve(killed) == NULL);
	CHECK(buf_handle(0).id != killed.id);
	CHECK(buf_handle(0).generation != killed.generation);

	/* A handle taken on a slot that has never been used names nothing. */
	for (i = 0; i < MAX_BUFFERS; i++) {
		if (!buflist[i].active) {
			CHECK(buf_resolve(buf_handle(i)) == NULL);
			break;
		}
	}

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* A special buffer that takes over a freed slot gets a fresh identity too:
 * buf_reset_slot() is the other way a slot changes hands. */
static void test_special_buffer_reuse_bumps_identity(void)
{
	char *names[1];
	struct kg_buffer_handle before;
	int target;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	target
	    = buf_prepare_special_text("*background*", &compilation_syntax, 1);
	CHECK(target > 0);
	before = buf_handle(target);
	CHECK(before.id != 0);

	/* Re-preparing the same name keeps the buffer, so the handle holds. */
	CHECK(buf_prepare_special_text("*background*", &compilation_syntax, 1)
	    == target);
	CHECK(buf_resolve(before) == &buflist[target]);

	/* A different special buffer taking a free slot gets its own id. */
	CHECK(buf_handle(
		  buf_prepare_special_text("*other*", &compilation_syntax, 1))
		  .id
	    != before.id);

	session_teardown();
	free(names[0]);
}

/* The goal column is the one view field no record owns: it lives on the
 * session and therefore leaks across a buffer switch.  Pinned here so the
 * phase that gives the view an owner has to change this test on purpose. */
static void test_goal_column_is_session_scoped_today(void)
{
	char *names[2];

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	editor.desired_visual_col = 7;
	buf_save_current_state();
	buf_restore_from_slot(1);
	CHECK(editor.desired_visual_col == 7);

	win_split_horizontal();
	win_cycle_next();
	CHECK(editor.desired_visual_col == 7);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

int main(void)
{
	char dir_template[] = "test_winmgr_XXXXXX";
	char *dir = mkdtemp(dir_template);
	char cmd[sizeof(tmpdir) + 16];

	if (!dir) {
		fprintf(stderr, "mkdtemp failed\n");
		return 1;
	}
	snprintf(tmpdir, sizeof(tmpdir), "%s", dir);

	printf("test_winmgr\n");
	RUN(test_two_buffers_own_disjoint_text);
	RUN(test_switch_round_trip_keeps_fields_in_their_buffer);
	RUN(test_split_shares_text_not_point);
	RUN(test_vertical_split_geometry);
	RUN(test_display_other_window_retargets_only_that_window);
	RUN(test_delete_window_paths);
	RUN(test_kill_buffer_shown_twice);
	RUN(test_slot_exhaustion_and_reuse);
	RUN(test_append_to_hidden_buffer_leaves_current_alone);
	RUN(test_append_to_visible_buffer_follows_that_window);
	RUN(test_reflow_sizes);
	RUN(test_handles_do_not_survive_their_buffer);
	RUN(test_special_buffer_reuse_bumps_identity);
	RUN(test_goal_column_is_session_scoped_today);

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "warning: could not remove %s\n", tmpdir);
	}
	return test_summary();
}
