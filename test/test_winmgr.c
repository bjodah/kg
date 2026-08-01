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
#include "../src/event.h"
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

/* The record for buffer slot `i`.  There is nothing to flush first any
 * more: a buffer owns its state whether or not it is the current one. */
static struct editor_buffer *bslot(int i) { return &buflist[i]; }

/* The record for window slot `i`, likewise. */
static struct editor_window *wslot(int i) { return &winlist[i]; }

static int set_mark_ring(struct editor_buffer *b, int index, int row, int col)
{
	struct kg_marker_handle marker;

	if (index < 0 || index >= MARK_RING_MAX) {
		return 0;
	}
	marker = kg_marker_create(
	    b, buffer_row_col_to_position(b, row, col), KG_MARKER_GRAV_LEFT);
	if (marker.id == 0) {
		return 0;
	}
	b->mark_ring[index] = (struct kg_buffer_mark) {
		.marker = marker,
		.virtual_chars_col = -1,
	};
	if (b->mark_ring_len <= index) {
		b->mark_ring_len = index + 1;
	}
	return 1;
}

static int mark_ring_col(const struct editor_buffer *b, int index)
{
	int row, col;

	return index >= 0 && index < b->mark_ring_len
		&& kg_marker_get_row_col(b->mark_ring[index].marker, &row, &col)
	    ? col
	    : -1;
}

/* A fresh session with one window and the named files open, current buffer 0.
 */
static void session(int nfiles, char **names)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		buflist[i].active = 0;
	}
	/* This suite links the real bufmgr.c/winmgr.c, so every open, kill
	 * and view attach/detach below is a real lifecycle producer, and
	 * this file never registers a subscriber -- so a drain with nobody
	 * listening is what keeps a later test's opens from being refused
	 * by an earlier test's undrained events, the same relief
	 * src/main.c's safe points give a real session. */
	kg_event_drain_safe();
	reset_current_buffer();
	reset_current_view();
	memset(&editor, 0, sizeof(editor));
	bcur()->readonly_override = -1;
	undo_stack_init(&bcur()->undostack);
	win_total_rows = 24;
	win_total_cols = 80;
	buf_load_args(nfiles, names, 0);
	win_init();
}

/* editor_cleanup() runs at most once per process, so this suite frees its
 * own sessions.  Every slot owns its rows, its filename and its undo chain,
 * which is exactly the list a leak here would prove wrong. */
static void session_teardown(void)
{
	int i, j;

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
		kg_marker_store_free(b);
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
	reset_current_buffer();
	reset_current_view();
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

/* ---- The model ----
 *
 * The same six invariants src/winmgr.c checks when KG_DEBUG_STATE is
 * armed, asked here in every build.  `invariants()` is what turns an
 * example-based case into a model-style one: a case drives a lifecycle
 * operation and then asserts nothing in particular went wrong, which is
 * the half an outcome-only assertion never covers.
 *
 * The seventh invariant, buffer-owned marker and decoration handles
 * resolving to their buffer, has nothing to check yet: markers arrive
 * with Plan 03.
 */
static void invariants(void)
{
	int i, j, buffers = 0, windows = 0;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];

		if (!b->active) {
			continue;
		}
		buffers++;
		CHECK(b->id != 0);
		CHECK(b->numrows >= 0);
		CHECK(b->row_capacity >= b->numrows);
		CHECK(b->row != NULL || b->numrows == 0);
		for (j = i + 1; j < MAX_BUFFERS; j++) {
			if (!buflist[j].active) {
				continue;
			}
			CHECK(buflist[j].id != b->id);
			CHECK(b->row == NULL || buflist[j].row != b->row);
		}
	}
	CHECK(buffers == buf_count);

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		windows++;
		CHECK(win_buffer(&winlist[i]) != NULL);
	}
	CHECK(windows == win_count);
	CHECK(win_count == 0 || win_buffer(wcur()) == bcur());
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
	wcur()->cy = 1;
	wcur()->cx = 2;
	editor_insert_char('X');
	CHECK(test_set_mark(bcur(), 0, 3));
	editor_set_local_readonly(1);
	snprintf(bcur()->compile_command, sizeof(bcur()->compile_command),
	    "make zero");
	bcur()->compile_command_user_override = 1;
	rows0 = bslot(0)->row;

	CHECK(bslot(0)->dirty != 0);
	CHECK(undo_depth(bslot(0)) == 1);

	/* Switch to buffer 1. */
	buf_select(1);

	CHECK(buf_current == 1);
	CHECK(bcur()->dirty == 0);
	CHECK(!kg_mark_is_set(bcur()));
	CHECK(bcur()->readonly == 0);
	CHECK(strcmp(bcur()->compile_command, "make -k") == 0);
	CHECK(bcur()->compile_command_user_override == 0);
	CHECK(bcur()->undostack.size == 0);
	CHECK(bcur()->row != rows0);

	/* Buffer 0's record kept everything. */
	CHECK(buflist[0].dirty != 0);
	CHECK(kg_mark_is_set(&buflist[0]));
	CHECK(test_mark_col(&buflist[0]) == 3);
	CHECK(buflist[0].readonly_local == 1);
	CHECK(strcmp(buflist[0].compile_command, "make zero") == 0);
	CHECK(buflist[0].undostack.size == 1);
	CHECK(buflist[0].row == rows0);

	/* Edit buffer 1 so it has its own history, then switch back. */
	editor_insert_char('Y');
	buf_select(0);

	CHECK(buf_current == 0);
	CHECK(bcur()->row == rows0);
	CHECK(bcur()->dirty != 0);
	CHECK(kg_mark_is_set(bcur()) && test_mark_col(bcur()) == 3);
	CHECK(bcur()->readonly == 1);
	CHECK(strcmp(bcur()->compile_command, "make zero") == 0);
	CHECK(bcur()->undostack.size == 1);
	CHECK(buflist[1].undostack.size == 1);
	CHECK(buflist[1].dirty != 0);
	CHECK(!kg_mark_is_set(&buflist[1]));

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
	invariants();
	other = other_window();
	CHECK(other >= 0);
	/* A split copies the handle, deliberately: two views of one
	 * buffer, not two names for it. */
	CHECK(win_buffer(&winlist[win_current]) == win_buffer(&winlist[other]));

	/* Move point in the selected window only. */
	wcur()->cy = 2;
	wcur()->cx = 1;
	CHECK(wslot(win_current)->cy == 2);
	CHECK(winlist[other].cy == 0);

	/* Now select the other window and confirm it kept its own point. */
	win_cycle_next();
	CHECK(win_current == other);
	CHECK(wcur()->cy == 0);
	CHECK(wcur()->cx == 0);
	/* Same buffer, same rows. */
	CHECK(bcur()->row == win_buffer(&winlist[win_current])->row);

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
	CHECK(wcur()->w == winlist[win_current].w);
	CHECK(wcur()->h == winlist[win_current].h);

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
	invariants();
	other = other_window();
	CHECK(other >= 0);
	CHECK(win_buffer_slot(&winlist[other]) == 1);
	CHECK(win_buffer_slot(&winlist[win_current]) == 0);
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
	invariants();
	CHECK(win_count == 1);
	CHECK(winlist[win_current].active);
	CHECK(win_buffer_slot(&winlist[win_current]) == 1);
	CHECK(buf_current == 1);
	CHECK(wcur()->h == winlist[win_current].h);

	win_split_horizontal();
	CHECK(win_count == 2);
	win_delete_others(); /* C-x 1 */
	invariants();
	CHECK(win_count == 1);
	CHECK(winlist[win_current].active);
	CHECK(buf_current == 1);
	CHECK(wcur()->h == winlist[win_current].h);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* C-x 1 discards every other window.  A window's point is the only record
 * of where the buffer it shows was being read, so a discarded window banks
 * it the way C-x 0 does -- otherwise showing that buffer again resumes
 * wherever some earlier view left it. */
static void test_delete_others_banks_the_views_it_discards(void)
{
	char *names[2];
	int other;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "b0\nb1\nb2\nb3\nb4\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);

	/* Read b.txt in the other window, four lines down. */
	win_cycle_next();
	CHECK(win_current == other);
	buf_select(1);
	wcur()->cy = 3;
	wcur()->cx = 1;

	/* Back to the first window, then C-x 1. */
	win_cycle_next();
	CHECK(win_current != other);
	CHECK(buf_current == 0);
	win_delete_others();
	invariants();
	CHECK(win_count == 1);
	CHECK(!winlist[other].active);

	/* Showing b.txt again resumes at the point the discarded window
	 * had in it. */
	CHECK(buflist[1].last_point.cy == 3);
	buf_select(1);
	CHECK(buf_current == 1);
	CHECK(wcur()->cy == 3);
	CHECK(wcur()->cx == 1);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* ---- Auto-revert's restriction ----
 *
 * The poll walks every buffer but reverts only the current one; the rest
 * are flagged and reload at the next buf_select().  These four cases are
 * what widening that restriction would have to survive, so they say what
 * the reload does to a view before anything decides to do it to more
 * views.  `autorevert_poll()` rate-limits itself with a process-lifetime
 * static, so exactly one test here may call it; the others set
 * `disk_changed` by hand, which is the poll's whole output. */

/* One poll, two changed files: only the current buffer's text moves. */
static void test_autorevert_poll_reverts_only_the_current_buffer(void)
{
	char *names[2];

	write_text_file(tmppath("a.txt"), "a0\na1\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "b0\nb1\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	global_auto_revert = 1;
	CHECK(buf_current == 0);

	write_text_file(tmppath("a.txt"), "A0\nA1\nA2\n");
	write_text_file(tmppath("b.txt"), "B0\nB1\nB2\n");
	CHECK(autorevert_poll() == 1);

	CHECK(buflist[0].numrows == 4);
	CHECK(memcmp(buflist[0].row[0].chars, "A0", 2) == 0);
	CHECK(buflist[0].disk_changed == 0);

	/* The other buffer is flagged and left holding its old text. */
	CHECK(buflist[1].disk_changed == 1);
	CHECK(buflist[1].numrows == 3);
	CHECK(memcmp(buflist[1].row[0].chars, "b0", 2) == 0);

	/* Showing it is what reloads it. */
	buf_select(1);
	CHECK(buflist[1].numrows == 4);
	CHECK(memcmp(buflist[1].row[0].chars, "B0", 2) == 0);
	CHECK(buflist[1].disk_changed == 0);

	global_auto_revert = 0;
	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* A point banked past the new end of file comes back inside it. */
static void test_deferred_revert_clamps_point_past_new_eof(void)
{
	char *names[2];

	write_text_file(tmppath("a.txt"), "a0\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(
	    tmppath("b.txt"), "b0\nb1\nb2\nb3\nb4\nb5\nb6\nb7\nb8\nb9\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);

	buf_select(1);
	wcur()->cy = 8;
	wcur()->cx = 2;
	buf_select(0);
	CHECK(buflist[1].last_point.cy == 8);

	write_text_file(tmppath("b.txt"), "B0\n");
	buflist[1].disk_changed = 1;
	buflist[1].auto_revert = 1;

	buf_select(1);
	CHECK(buflist[1].numrows == 2);
	CHECK(wcur()->rowoff + wcur()->cy < buflist[1].numrows);
	CHECK(wcur()->coloff + wcur()->cx
	    <= buflist[1].row[wcur()->rowoff + wcur()->cy].size);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* A revert drops the region it finds.  Nothing asks first, and nothing
 * says so afterwards beyond the status line. */
static void test_revert_drops_the_region_it_finds(void)
{
	char *names[1];

	write_text_file(tmppath("a.txt"), "a0\na1\na2\n");
	names[0] = strdup(tmppath("a.txt"));

	session(1, names);
	CHECK(test_set_mark(bcur(), 0, 1));
	bcur()->mark_highlight = 1;
	bcur()->rect_mode = 1;
	CHECK(set_mark_ring(bcur(), 0, 1, 1));

	write_text_file(tmppath("a.txt"), "A0\nA1\nA2\n");
	bcur()->disk_changed = 1;
	bcur()->auto_revert = 1;
	buf_select(0);

	CHECK(memcmp(bcur()->row[0].chars, "A0", 2) == 0);
	CHECK(!kg_mark_is_set(bcur()));
	CHECK(bcur()->mark_highlight == 0);
	CHECK(bcur()->rect_mode == 0);
	CHECK(bcur()->mark_ring_len == 0);
	CHECK(bcur()->undostack.head == NULL);

	session_teardown();
	free(names[0]);
}

/* A revert clamps every window on the buffer, not just the selected one.
 * buf_attach_view() has nothing to do for a window already on the buffer,
 * so an unclamped one would stay past the new end of file until the user
 * typed there -- and typing at a row that does not exist appends the
 * blank lines to reach it. */
static void test_revert_clamps_every_window_on_the_buffer(void)
{
	char *names[1];
	int other;

	write_text_file(
	    tmppath("a.txt"), "a0\na1\na2\na3\na4\na5\na6\na7\na8\na9\n");
	names[0] = strdup(tmppath("a.txt"));

	session(1, names);
	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);
	winlist[other].cy = 9;
	winlist[other].rowoff = 0;
	wcur()->cy = 1;

	write_text_file(tmppath("a.txt"), "A0\n");
	bcur()->disk_changed = 1;
	bcur()->auto_revert = 1;
	buf_select(0);

	CHECK(bcur()->numrows == 2);
	CHECK(wcur()->rowoff + wcur()->cy < bcur()->numrows);
	CHECK(winlist[other].rowoff + winlist[other].cy < bcur()->numrows);

	win_cycle_next();
	CHECK(win_current == other);
	CHECK(wcur()->rowoff + wcur()->cy < bcur()->numrows);

	session_teardown();
	free(names[0]);
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
	CHECK(win_buffer_slot(&winlist[other]) == 0);

	buf_kill(-1);
	invariants();
	CHECK(buf_count == 1);
	CHECK(!buflist[0].active);
	CHECK(buflist[0].row == NULL);
	CHECK(buflist[0].row_capacity == 0);
	CHECK(buflist[0].filename == NULL);
	CHECK(buf_current == 1);
	CHECK(win_buffer_slot(&winlist[win_current]) == 1);
	/* The unselected window comes off the killed buffer too, and lands
	 * on the survivor by identity: the handle it held stopped resolving
	 * when the buffer died, so it cannot instead start naming whoever
	 * takes the slot next. */
	CHECK(win_buffer_slot(&winlist[other]) == 1);
	CHECK(win_buffer(&winlist[other]) == &buflist[1]);

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
	size_t marker_pos;
	struct kg_marker_handle killed_marker;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	for (i = 1; i < MAX_BUFFERS; i++) {
		snprintf(name, sizeof(name), "f%02d.txt", i);
		write_text_file(tmppath(name), "x\n");
		buf_open_path(tmppath(name), 0);
	}
	CHECK(buf_count == MAX_BUFFERS);
	invariants();
	/* Filling every slot is ~2 lifecycle events each (open, then the
	 * window following it); drain them here, the same relief a real
	 * session's safe points give, so the kill/reopen below is not the
	 * operation that happens to hit the reserved capacity's ceiling. */
	kg_event_drain_safe();

	/* One more is refused rather than overwriting a slot. */
	write_text_file(tmppath("overflow.txt"), "x\n");
	buf_open_path(tmppath("overflow.txt"), 0);
	CHECK(buf_count == MAX_BUFFERS);
	invariants();

	/* Kill slot 5's buffer and reopen: the freed slot is taken. */
	buf_select(5);
	CHECK(bslot(5)->row != NULL);
	CHECK(bslot(5)->filename != NULL);
	killed_marker = kg_marker_create(bcur(), 0, KG_MARKER_GRAV_LEFT);
	CHECK(killed_marker.id != 0);
	CHECK(kg_mark_set_position(bcur(), 0));
	buf_kill(-1);
	CHECK(buf_count == MAX_BUFFERS - 1);
	CHECK(!buflist[5].active);
	CHECK(buflist[5].markers == NULL);
	CHECK(kg_marker_resolve(killed_marker, &marker_pos) == KG_MARKER_GONE);

	buf_open_path(tmppath("overflow.txt"), 0);
	CHECK(buf_count == MAX_BUFFERS);
	invariants();
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
	CHECK(!kg_mark_is_set(bslot(5)));
	CHECK(kg_marker_resolve(killed_marker, &marker_pos) == KG_MARKER_GONE);

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

	wcur()->cy = 1;
	editor_insert_char('Q');
	rows0 = bcur()->row;
	filename0 = bcur()->filename;

	target
	    = buf_prepare_special_text("*background*", &compilation_syntax, 1);
	CHECK(target > 0);
	CHECK(target != buf_current);
	CHECK(buf_append_special_text(target, "line one\nline two\n", 18) == 0);

	CHECK(buf_current == 0);
	CHECK(bcur()->row == rows0);
	CHECK(bcur()->filename == filename0);
	CHECK(bcur()->numrows == 3);
	CHECK(wcur()->cy == 1);
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
	CHECK(win_buffer_slot(&winlist[other]) == target);

	wcur()->cy = 1;
	CHECK(buf_append_special_text(target, "one\ntwo\nthree\n", 14) == 0);

	CHECK(buf_current == 0);
	CHECK(wcur()->cy == 1);
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
	CHECK(wcur()->h == 8);
	CHECK(wcur()->w == 40);

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

/* The mark, the mark ring and the region flags belong to the buffer.  A
 * buffer switch must leave each buffer's region as its own, and the window
 * that shows a buffer must not be able to own a second one. */
static void test_marks_belong_to_the_buffer(void)
{
	char *names[2];
	int other;

	write_text_file(tmppath("a.txt"), "alpha\nbeta\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);

	/* Buffer 0: a live region and two entries in the mark ring. */
	CHECK(test_set_mark(bcur(), 0, 2));
	bcur()->mark_highlight = 1;
	bcur()->shift_select = 1;
	bcur()->rect_mode = 1;
	CHECK(set_mark_ring(bcur(), 0, 1, 4));

	buf_select(1);

	/* Buffer 1 has a region of its own, which is to say none. */
	CHECK(!kg_mark_is_set(bcur()));
	CHECK(bcur()->mark_highlight == 0);
	CHECK(bcur()->shift_select == 0);
	CHECK(bcur()->rect_mode == 0);
	CHECK(bcur()->mark_ring_len == 0);
	CHECK(kg_mark_is_set(&buflist[0]));
	CHECK(test_mark_col(&buflist[0]) == 2);
	CHECK(buflist[0].mark_ring_len == 1);
	CHECK(mark_ring_col(&buflist[0], 0) == 4);

	buf_select(0);
	CHECK(kg_mark_is_set(bcur()));
	CHECK(test_mark_col(bcur()) == 2);
	CHECK(bcur()->mark_highlight == 1);
	CHECK(bcur()->rect_mode == 1);
	CHECK(bcur()->mark_ring_len == 1);

	/* One buffer, two windows, one region: the second window sees the
	 * mark the first one set, because the buffer holds it. */
	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);
	win_cycle_next();
	CHECK(win_current == other);
	CHECK(buf_current == 0);
	CHECK(kg_mark_is_set(bcur()));
	CHECK(test_mark_col(bcur()) == 2);

	session_teardown();
	free(names[0]);
	free(names[1]);
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

/* A zeroed handle names nothing, whatever sits in slot 0.  This is the
 * spelling a detached view uses, so it has to be a definite "no". */
static void test_zeroed_handle_names_nothing(void)
{
	char *names[1];
	struct kg_buffer_handle zero = { 0, 0, 0 };

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	CHECK(buflist[0].active);
	CHECK(buflist[0].id != 0);
	CHECK(buf_resolve(zero) == NULL);
	CHECK(buf_handle_slot(zero) == -1);

	/* Identity is 64-bit and monotonic: it is wide enough that the
	 * counter's claim to never repeat is a fact rather than a hope. */
	CHECK(sizeof(buflist[0].id) == 8);
	CHECK(buf_handle(0).id == buflist[0].id);

	session_teardown();
	free(names[0]);
}

/* The safe-point sweep is the net under every path that ends a buffer.
 * A window holding a handle nobody detached must not be drawn from, and
 * must not be left to resurrect the slot the next time it is selected:
 * the sweep puts it on the buffer the session is in and says so. */
static void test_check_handles_recovers_an_injected_stale_view(void)
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
	buf_select(1);
	CHECK(win_buffer_slot(wcur()) == 1);

	/* Inject what no code path is allowed to produce: the other window
	 * keeps a handle on slot 0 while slot 0 changes hands. */
	CHECK(win_buffer_slot(&winlist[other]) == 0);
	buflist[0].generation++;
	CHECK(win_buffer(&winlist[other]) == NULL);

	win_check_handles();
	invariants();
	CHECK(win_buffer(&winlist[other]) == &buflist[1]);
	CHECK(win_buffer_slot(&winlist[other]) == 1);
	/* The selected window was never stale and is untouched. */
	CHECK(win_buffer_slot(wcur()) == 1);

	/* And it is idempotent: a second sweep finds nothing to do. */
	win_check_handles();
	CHECK(win_buffer_slot(&winlist[other]) == 1);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* Killing the last buffer ends the session, and is the one commit the
 * invariants deliberately do not describe: there is no current buffer for
 * a window to be showing.  What must still hold is that no window is left
 * holding a *stale* handle -- one that names a slot somebody else could
 * take.  A detached view names nothing, which is a different thing. */
static void test_last_buffer_leaves_no_stale_view(void)
{
	char *names[1];
	int i, live = 0;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));

	session(1, names);
	win_split_horizontal();
	CHECK(win_count == 2);
	invariants();

	running = 1;
	buf_kill(-1);
	CHECK(buf_count == 0);
	CHECK(running == 0);

	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		live++;
		CHECK(win_buffer(&winlist[i]) == NULL);
		CHECK(win_buffer_slot(&winlist[i]) == -1);
		/* Detached, not stale: the handle names no slot at all. */
		CHECK(winlist[i].buf.id == 0);
	}
	CHECK(live == 2);

	running = 1;
	session_teardown();
	free(names[0]);
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

/* buf_current is the selected window's buffer, by construction.  Every
 * command that changes either one has to leave them agreeing, or the mode
 * line describes one buffer while the keys edit another. */
static void test_current_buffer_is_the_selected_window_s(void)
{
	char *names[2];
	int i;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	buf_select(1);
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	win_split_horizontal();
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	win_display_buffer_other_window(0);
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	win_cycle_next();
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	win_split_vertical();
	win_cycle_next();
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	win_delete_current();
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	win_delete_others();
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	buf_kill(-1);
	CHECK(buf_current == win_buffer_slot(wcur()));
	invariants();

	/* And every window still names a buffer that exists -- a live one,
	 * not merely a slot number in range: a window left on a killed slot
	 * resurrects it, uncounted, the moment it is selected. */
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (winlist[i].active) {
			CHECK(win_buffer(&winlist[i]) != NULL);
			CHECK(win_buffer_slot(&winlist[i]) >= 0);
			CHECK(win_buffer(&winlist[i])
			    == &buflist[win_buffer_slot(&winlist[i])]);
		}
	}

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* The goal column belongs to the view.  It is only meaningful between two
 * consecutive vertical motions in one buffer, so attaching a view to a
 * different buffer drops it -- and a different window has its own. */
static void test_goal_column_belongs_to_the_view(void)
{
	char *names[2];
	int other;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	wcur()->desired_visual_col = 7;
	buf_select(1);
	CHECK(wcur()->desired_visual_col == -1);

	/* A split copies the view, goal column and all -- the new window is
	 * looking at the same place in the same buffer. */
	wcur()->desired_visual_col = 5;
	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);
	CHECK(winlist[other].desired_visual_col == 5);

	/* But the two are separate from then on. */
	winlist[other].desired_visual_col = 9;
	CHECK(wcur()->desired_visual_col == 5);
	win_cycle_next();
	CHECK(wcur()->desired_visual_col == 9);

	session_teardown();
	free(names[0]);
	free(names[1]);
}

/* A window handle is win_handle()'s counterpart to buf_handle(): live while
 * the slot it names is the same window, dead once that slot is released,
 * and still dead once the slot is handed to a different window. */
static void test_window_handle_resolves_until_slot_released_and_reused(void)
{
	char *names[1];
	int other, reused;
	struct kg_window_handle h;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);
	h = win_handle(other);
	CHECK(h.id != 0);
	CHECK(win_resolve(h) == &winlist[other]);
	CHECK(win_handle_slot(h) == other);

	win_cycle_next();
	CHECK(win_current == other);
	win_delete_current();
	CHECK(win_resolve(h) == NULL);
	CHECK(win_handle_slot(h) == -1);

	/* The freed slot comes back as somebody else's window; the old
	 * handle must not resolve to the new occupant either. */
	win_split_horizontal();
	reused = other_window();
	CHECK(reused == other);
	CHECK(win_resolve(h) == NULL);
	CHECK(win_handle(reused).id != h.id);
	CHECK(win_resolve(win_handle(reused)) == &winlist[reused]);

	session_teardown();
	free(names[0]);
}

/* A split's new window is a new view, not a second name for the one it was
 * copied from: same buffer, same point, but its own identity. */
static void test_split_gives_the_new_window_a_distinct_identity(void)
{
	char *names[1];
	int other, i;
	struct kg_window_handle h0, h1;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	h0 = win_handle(win_current);
	CHECK(h0.id != 0);

	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);
	h1 = win_handle(other);

	CHECK(h1.id != 0);
	CHECK(h1.id != h0.id);
	CHECK(win_resolve(h0) == &winlist[win_current]);
	CHECK(win_resolve(h1) == &winlist[other]);

	/* Splitting again (neither split moves focus, so win_current is
	 * still h0's window) must not repeat an id either. */
	win_split_vertical();
	CHECK(win_count == 3);
	for (i = 0; i < MAX_WINDOWS; i++) {
		struct kg_window_handle h;

		if (!winlist[i].active || i == win_current || i == other) {
			continue;
		}
		h = win_handle(i);
		CHECK(h.id != 0);
		CHECK(h.id != h0.id);
		CHECK(h.id != h1.id);
	}

	session_teardown();
	free(names[0]);
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
	RUN(test_delete_others_banks_the_views_it_discards);
	RUN(test_autorevert_poll_reverts_only_the_current_buffer);
	RUN(test_deferred_revert_clamps_point_past_new_eof);
	RUN(test_revert_drops_the_region_it_finds);
	RUN(test_revert_clamps_every_window_on_the_buffer);
	RUN(test_kill_buffer_shown_twice);
	RUN(test_slot_exhaustion_and_reuse);
	RUN(test_append_to_hidden_buffer_leaves_current_alone);
	RUN(test_append_to_visible_buffer_follows_that_window);
	RUN(test_reflow_sizes);
	RUN(test_marks_belong_to_the_buffer);
	RUN(test_handles_do_not_survive_their_buffer);
	RUN(test_zeroed_handle_names_nothing);
	RUN(test_check_handles_recovers_an_injected_stale_view);
	RUN(test_last_buffer_leaves_no_stale_view);
	RUN(test_special_buffer_reuse_bumps_identity);
	RUN(test_current_buffer_is_the_selected_window_s);
	RUN(test_goal_column_belongs_to_the_view);
	RUN(test_window_handle_resolves_until_slot_released_and_reused);
	RUN(test_split_gives_the_new_window_a_distinct_identity);

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "warning: could not remove %s\n", tmpdir);
	}
	return test_summary();
}
