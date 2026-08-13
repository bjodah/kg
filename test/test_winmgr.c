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

#include "../src/bufmgr.h"
#include "../src/bufmgr_internal.h"
#include "../src/def.h"
#include "../src/event.h"
#include "../src/vgeom.h"
#include "../src/winmgr.h"
#include "test.h"
#include <limits.h>
#include <stdint.h>
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

	for (i = 0; i < MAX_WINDOWS; i++) {
		free(winlist[i].vgeom);
		winlist[i].vgeom = NULL;
	}

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

/* Drain every remaining event into `out` (capacity `max`), in publication
 * order.  Returns the count -- the same helper test_event.c uses, kept
 * local here since this suite links the real producers rather than
 * calling the queue's constructors by hand. */
static size_t drain_all(struct kg_event *out, size_t max)
{
	size_t n = 0;

	while (n < max && kg_event_queue_pop(&out[n])) {
		n++;
	}
	return n;
}

static void check_snapshot_view(const struct kg_window_snapshot *expected,
    const struct editor_window *actual)
{
	CHECK(win_shows_buffer(actual, expected->buffer));
	CHECK(actual->cx == expected->cx);
	CHECK(actual->cy == expected->cy);
	CHECK(actual->rowoff == expected->rowoff);
	CHECK(actual->coloff == expected->coloff);
	CHECK(actual->rowoff_visual == expected->rowoff_visual);
	CHECK(actual->desired_visual_col == expected->desired_visual_col);
	CHECK(actual->col_group == expected->col_group);
}

static int same_window_handle(
    struct kg_window_handle a, struct kg_window_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

static void check_configuration_view_restored(
    const struct kg_window_configuration *configuration)
{
	CHECK(win_count == configuration->count);
	for (int i = 0; i < configuration->count; i++) {
		const struct kg_window_snapshot *snapshot
		    = &configuration->windows[i];

		CHECK(winlist[snapshot->slot].active);
		check_snapshot_view(snapshot, &winlist[snapshot->slot]);
	}
	CHECK(win_current == configuration->selected_window.slot);
	CHECK(buf_current == win_buffer_slot(wcur()));
}

static void fill_buffer_handles(struct kg_buffer_handle handles[], int count)
{
	for (int i = 0; i < count; i++) {
		handles[i] = buf_handle(i);
		CHECK(buf_resolve(handles[i]) != NULL);
	}
}

static void session_with_buffers(int count, char *names[])
{
	static int serial;
	char leaf[48];
	const char *text
	    = "line0\nline1\nline2\nline3\nline4\nline5\nline6\nline7\n";

	for (int i = 0; i < count; i++) {
		const char *path;

		snprintf(leaf, sizeof(leaf), "layout-%d-%d.txt", serial, i);
		path = tmppath(leaf);
		write_text_file(path, text);
		names[i] = strdup(path);
		CHECK(names[i] != NULL);
	}
	serial++;
	session(count, names);
}

static void teardown_buffer_session(int count, char *names[])
{
	session_teardown();
	for (int i = 0; i < count; i++) {
		free(names[i]);
	}
}

struct layout_observation {
	struct kg_window_configuration configuration;
	struct kg_window_handle handles[MAX_WINDOWS];
	int geometry[MAX_WINDOWS][5];
	int current;
	int buffer_current;
	int count;
	void *selected_vgeom;
};

static struct layout_observation observe_layout(void)
{
	struct layout_observation observed = { 0 };

	win_configuration_save(&observed.configuration);
	observed.current = win_current;
	observed.buffer_current = buf_current;
	observed.count = win_count;
	observed.selected_vgeom = wcur()->vgeom;
	for (int i = 0; i < MAX_WINDOWS; i++) {
		observed.handles[i] = win_handle(i);
		observed.geometry[i][0] = winlist[i].active;
		observed.geometry[i][1] = winlist[i].x;
		observed.geometry[i][2] = winlist[i].y;
		observed.geometry[i][3] = winlist[i].w;
		observed.geometry[i][4] = winlist[i].h;
	}
	return observed;
}

static void check_layout_unchanged(const struct layout_observation *before)
{
	check_configuration_view_restored(&before->configuration);
	CHECK(win_current == before->current);
	CHECK(buf_current == before->buffer_current);
	CHECK(win_count == before->count);
	CHECK(wcur()->vgeom == before->selected_vgeom);
	for (int i = 0; i < MAX_WINDOWS; i++) {
		CHECK(same_window_handle(win_handle(i), before->handles[i]));
		CHECK(winlist[i].active == before->geometry[i][0]);
		CHECK(winlist[i].x == before->geometry[i][1]);
		CHECK(winlist[i].y == before->geometry[i][2]);
		CHECK(winlist[i].w == before->geometry[i][3]);
		CHECK(winlist[i].h == before->geometry[i][4]);
	}
	CHECK(kg_event_queue_pop(NULL) == false);
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

/* One marker handle a live buffer is supposed to own: `owner`'s mark, a
 * mark ring entry, or a decoration endpoint.  Mirrors
 * src/winmgr.c's check_marker_owner() (the seventh invariant), but with
 * CHECK() rather than an abort: this model runs in every build, including
 * ones without KG_DEBUG_STATE. */
static void model_check_marker_owner(
    struct kg_marker_handle m, struct kg_buffer_handle owner)
{
	if (!m.id) {
		return;
	}
	CHECK(m.buffer.slot == owner.slot && m.buffer.id == owner.id
	    && m.buffer.generation == owner.generation);
	CHECK(kg_marker_resolve(m, NULL) == KG_MARKER_OK);
}

/* ---- The model ----
 *
 * The same seven invariants src/winmgr.c checks when KG_DEBUG_STATE is
 * armed, asked here in every build.  `invariants()` is what turns an
 * example-based case into a model-style one: a case drives a lifecycle
 * operation and then asserts nothing in particular went wrong, which is
 * the half an outcome-only assertion never covers.
 */
static void invariants(void)
{
	int i, j, buffers = 0, windows = 0;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct editor_buffer *b = &buflist[i];
		struct kg_buffer_handle self;
		size_t k;

		if (!b->active) {
			continue;
		}
		buffers++;
		CHECK(b->id != 0);
		CHECK(b->numrows >= 0);
		CHECK(b->row_capacity >= b->numrows);
		CHECK(b->row != NULL || b->numrows == 0);
		self = buf_handle(i);
		model_check_marker_owner(b->mark.marker, self);
		for (k = 0; k < (size_t)b->mark_ring_len; k++) {
			model_check_marker_owner(b->mark_ring[k].marker, self);
		}
		if (b->decorations) {
			for (k = 0; k < b->decorations->count; k++) {
				model_check_marker_owner(
				    b->decorations->items[k].start, self);
				model_check_marker_owner(
				    b->decorations->items[k].end, self);
			}
		}
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

/* win_undisplay_buffer() takes down somebody else's window and leaves the
 * buffer alive: what a transient listing does when it has stopped being
 * about where point is (src/lsp_complete.h).  The three cases it declines
 * are the ones that would surprise -- a buffer nothing is showing, the
 * only window there is, and the window the reader is standing in. */
static void test_undisplay_buffer_drops_the_window_not_the_buffer(void)
{
	char *names[2];

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	/* Nothing shows buffer 1 yet, and buffer 0's window is the only
	 * one: both are declines, and neither disturbs the session. */
	win_undisplay_buffer(1);
	win_undisplay_buffer(0);
	invariants();
	CHECK(win_count == 1);

	win_display_buffer_other_window(1);
	CHECK(win_count == 2);
	/* The current window is not one this may close. */
	win_undisplay_buffer(0);
	CHECK(win_count == 2);

	win_undisplay_buffer(1);
	invariants();
	CHECK(win_count == 1);
	CHECK(win_buffer_slot(&winlist[win_current]) == 0);
	CHECK(buf_current == 0);
	CHECK(wcur()->h == winlist[win_current].h);
	/* Closing a window is not killing a buffer: C-x b still reaches it. */
	CHECK(bslot(1)->active);
	CHECK(bslot(1)->numrows == 3);

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

/* Same scenario as test_kill_buffer_shown_twice(), but pinning the event
 * stream buf_kill_commit() promises in its own comment: KILLING published
 * once while the dying handle still resolves, one VIEW_DETACHED per window
 * that showed it, then KILLED once cleanup is done -- in that order, ahead
 * of anything buf_kill()'s post-cleanup reattach loop queues afterward. */
static void test_kill_shown_in_two_windows_publishes_killing_detach_killed(void)
{
	char *names[2];
	int home, other;
	struct kg_window_handle home_h, other_h;
	struct kg_buffer_handle dying;
	struct kg_event ev;
	int i;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	write_text_file(tmppath("b.txt"), "one\ntwo\n");
	names[1] = strdup(tmppath("b.txt"));

	session(2, names);
	win_split_horizontal();
	home = win_current;
	other = other_window();
	CHECK(other >= 0);
	CHECK(win_buffer_slot(&winlist[home]) == 0);
	CHECK(win_buffer_slot(&winlist[other]) == 0);
	home_h = win_handle(home);
	other_h = win_handle(other);
	dying = buf_handle(0);
	kg_event_drain_safe(); /* discard the session's own open/attach noise */

	buf_kill(-1);
	CHECK(buf_count == 1);

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_KILLING);
	CHECK(ev.payload.buffer_life.buffer.id == dying.id);

	bool saw_home = false, saw_other = false;
	for (i = 0; i < 2; i++) {
		CHECK(kg_event_queue_pop(&ev));
		CHECK(ev.kind == KG_EVENT_VIEW_DETACHED);
		CHECK(ev.payload.view.buffer.id == dying.id);
		if (ev.payload.view.window.id == home_h.id) {
			saw_home = true;
		} else if (ev.payload.view.window.id == other_h.id) {
			saw_other = true;
		}
	}
	CHECK(saw_home && saw_other);

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_KILLED);
	CHECK(ev.payload.buffer_life.buffer.id == dying.id);
	CHECK(!buf_resolve(ev.payload.buffer_life.buffer));

	/* No live window names the dead buffer, whatever buf_kill() queues
	 * next to put both windows back on the survivor. */
	CHECK(win_buffer(&winlist[home]) != NULL);
	CHECK(win_buffer(&winlist[other]) != NULL);
	CHECK(buf_resolve(winlist[home].buf) != buf_resolve(dying));
	CHECK(buf_resolve(winlist[other].buf) != buf_resolve(dying));
	for (i = 0; i < MAX_WINDOWS; i++) {
		if (!winlist[i].active) {
			continue;
		}
		CHECK(!win_shows_buffer(&winlist[i], dying));
	}

	kg_event_drain_safe();
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

	/* An open is ~3 lifecycle events (the outgoing view's detach, the
	 * incoming one's attach, the buffer's open) against a 64-slot queue,
	 * so filling the table drains as it goes -- the same relief a real
	 * session's per-keystroke safe point gives.  Without it the last
	 * slots are refused for want of event capacity, and this suite would
	 * be asserting that ceiling instead of the buffer table's. */
	for (i = 1; i < MAX_BUFFERS; i++) {
		snprintf(name, sizeof(name), "f%02d.txt", i);
		write_text_file(tmppath(name), "x\n");
		buf_open_path(tmppath(name), 0);
		kg_event_drain_safe();
	}
	CHECK(buf_count == MAX_BUFFERS);
	invariants();

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

/* The bound sizes buflist[] and every sibling table indexed by a buffer
 * slot, and only a full table reaches the last entry of any of them.  Fill
 * it, then take the buffer that landed in the last slot through the whole
 * lifecycle: resolve, display in another window, undisplay, kill, reuse,
 * and a change that collapses into that slot's event-overflow entry. */
static void test_last_slot_lifecycle(void)
{
	const int last = MAX_BUFFERS - 1;
	char *names[1];
	char name[64];
	struct kg_buffer_handle handle, reopened;
	struct kg_event ev;
	int i, other;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	/* Bounded by the iteration count as well as by buf_count, so an open
	 * that stops taking slots ends the loop instead of spinning; the
	 * drain is what keeps the event queue from being the thing that
	 * stops it (see test_slot_exhaustion_and_reuse()). */
	for (i = 1; i < MAX_BUFFERS && buf_count < MAX_BUFFERS; i++) {
		snprintf(name, sizeof(name), "f%02d.txt", i);
		write_text_file(tmppath(name), "x\n");
		buf_open_path(tmppath(name), 0);
		kg_event_drain_safe();
	}
	CHECK(buf_count == MAX_BUFFERS);
	CHECK(bslot(last)->active);
	invariants();

	/* One past the last slot is refused rather than folded onto one. */
	write_text_file(tmppath("over.txt"), "x\n");
	buf_open_path(tmppath("over.txt"), 0);
	CHECK(buf_count == MAX_BUFFERS);
	invariants();

	/* The last slot answers to a handle like any other. */
	handle = buf_handle(last);
	CHECK(buf_resolve(handle) == bslot(last));
	CHECK(buf_handle_slot(handle) == last);

	/* It displays in a second window, and stops being displayed.  The
	 * fill left it selected, and a buffer already on screen is not
	 * displayed again, so look away from it first. */
	CHECK(buf_select(0));
	kg_event_drain_safe();
	CHECK(win_can_display_buffer_other_window(last));
	win_display_buffer_other_window(last);
	other = other_window();
	CHECK(other >= 0);
	CHECK(win_buffer_slot(&winlist[other]) == last);
	win_undisplay_buffer(last);
	CHECK(win_count == 1);
	for (i = 0; i < MAX_WINDOWS; i++) {
		CHECK(!winlist[i].active
		    || !win_shows_buffer(&winlist[i], handle));
	}
	invariants();
	kg_event_drain_safe();

	/* Killing it frees the last slot, and it is the only free one, so
	 * the next open must take it back. */
	buf_select(last);
	buf_kill(-1);
	CHECK(buf_count == MAX_BUFFERS - 1);
	CHECK(!bslot(last)->active);
	CHECK(buf_resolve(handle) == NULL);
	buf_open_path(tmppath("over.txt"), 0);
	CHECK(buf_count == MAX_BUFFERS);
	CHECK(bslot(last)->active);
	CHECK(strstr(bslot(last)->filename, "over.txt") != NULL);
	reopened = buf_handle(last);
	CHECK(buf_resolve(reopened) == bslot(last));
	CHECK(buf_resolve(handle) == NULL);
	invariants();
	kg_event_drain_safe();

	/* With no droppable room left, a change to the reused buffer folds
	 * into the overflow entry the last slot owns, and the drain hands it
	 * back naming that buffer's current identity. */
	CHECK(buf_select(last));
	kg_event_drain_safe();
	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);
	editor_insert_char('Q');
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(ev.payload.broad.buffer.slot == last);
	CHECK(ev.payload.broad.buffer.id == reopened.id);
	CHECK(kg_event_queue_pop(NULL) == false);
	kg_event_queue_init();

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

/* A buffer the USER made under one of kg's own special names is not kg's
 * to rebuild.  C-x b takes any name at all (README), so `*compilation*` or
 * a debugger pane's name is a buffer somebody may have typed into -- and
 * buf_prepare_special_text() used to adopt it by name alone, dropping every
 * row and freeing the undo chain with it.  Ten callers reach that path, and
 * the debugger's four model panes reach it on a timer with no command
 * pressed at all.
 *
 * The flag goes with the slot, so killing the user's buffer hands the name
 * back and kg can make its own again. */
static void test_a_user_buffer_is_not_adopted_as_a_special_one(void)
{
	char *names[1];
	int mine;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	mine = buf_handle_slot(buf_create_named("*dap-stack*"));
	CHECK(mine >= 0);
	CHECK(buf_select(mine) != 0);
	editor_insert_char('h');
	editor_insert_char('i');
	CHECK(bslot(mine)->numrows == 1);
	CHECKF(bslot(mine)->undostack.head != NULL, "nothing to undo to begin");

	CHECKF(
	    buf_prepare_special_text("*dap-stack*", &compilation_syntax, 1) < 0,
	    "kg adopted a buffer of the user's");
	CHECKF(bslot(mine)->numrows == 1 && bslot(mine)->row[0].size == 2,
	    "the user's text went with the adoption");
	CHECKF(bslot(mine)->undostack.head != NULL,
	    "the user's undo chain went with the adoption");
	CHECKF(buf_find_special("*dap-stack*") < 0,
	    "the user's buffer answers to the special-buffer lookup");

	/* buf_kill_buffer() refuses a modified buffer outright (the prompt
	 * that would ask about it is buf_kill()'s), and what is under test
	 * here is the slot handover, not that policy. */
	bslot(mine)->dirty = 0;
	CHECK(buf_kill_buffer(buf_handle(mine)) != 0);
	kg_event_drain_safe();
	CHECKF(buf_prepare_special_text("*dap-stack*", &compilation_syntax, 1)
		>= 0,
	    "the name stayed refused after the user's buffer was killed");
	CHECK(buf_find_special("*dap-stack*") >= 0);

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

/* A fresh session's KG_EVENT_BUFFER_OPENED for its one buffer always
 * precedes the KG_EVENT_VIEW_ATTACHED that puts it in window 0:
 * buf_load_args() opens buffer 0 while win_count is still 0 (no window
 * exists yet to attach), and win_init()'s own buf_attach_view() is what
 * attaches it once the window does.  Loading the file's rows publishes its
 * own KG_EVENT_BUFFER_BROAD_CHANGE in between (kg_buffer_adopt_rows(),
 * called from editor_open()) -- expected, and not what this test is
 * about, so it only pins the relative order of open and attach, not
 * adjacency. */
static void test_open_precedes_attach_when_the_session_starts(void)
{
	char *names[1];
	struct kg_event out[8];
	size_t n, opened_at = SIZE_MAX, attached_at = SIZE_MAX;
	struct kg_buffer_handle opened = { 0 };

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));

	session(1, names);

	n = drain_all(out, 8);
	for (size_t i = 0; i < n; i++) {
		if (out[i].kind == KG_EVENT_BUFFER_OPENED
		    && opened_at == SIZE_MAX) {
			opened_at = i;
			opened = out[i].payload.buffer_life.buffer;
		}
		if (out[i].kind == KG_EVENT_VIEW_ATTACHED
		    && attached_at == SIZE_MAX) {
			attached_at = i;
		}
	}
	CHECK(opened_at != SIZE_MAX);
	CHECK(attached_at != SIZE_MAX);
	CHECK(opened_at < attached_at);
	CHECK(buf_resolve(opened) == &buflist[0]);
	CHECK(out[attached_at].payload.view.buffer.id == opened.id);
	CHECK(win_resolve(out[attached_at].payload.view.window)
	    == &winlist[win_current]);

	session_teardown();
	free(names[0]);
}

/* A window split publishes a KG_EVENT_VIEW_ATTACHED for the new view -- the
 * new window really is a second reader of the buffer, not merely a struct
 * copy the event stream never heard about -- and the attached window's own
 * handle is distinct from the one the split copied it from. */
static void test_split_publishes_view_attached_for_the_new_window(void)
{
	char *names[1];
	int other;
	struct kg_window_handle h0;
	struct kg_buffer_handle b0;
	struct kg_event ev;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);
	kg_event_drain_safe(); /* discard the session's own open/attach */

	h0 = win_handle(win_current);
	b0 = buf_handle(win_buffer_slot(&winlist[win_current]));

	win_split_horizontal();
	other = other_window();
	CHECK(other >= 0);

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_VIEW_ATTACHED);
	CHECK(ev.payload.view.window.id == win_handle(other).id);
	CHECK(ev.payload.view.window.id != h0.id);
	CHECK(ev.payload.view.buffer.id == b0.id);
	CHECK(win_resolve(ev.payload.view.window) == &winlist[other]);
	CHECK(kg_event_queue_pop(NULL) == false);

	kg_event_drain_safe();
	session_teardown();
	free(names[0]);
}

static void test_configuration_round_trip_resize_and_reuse(void)
{
	char *names[3];
	struct kg_buffer_handle buffers[3];
	struct kg_window_configuration configuration;
	struct kg_window_handle before[MAX_WINDOWS];
	int rows[] = { 2, 1 };

	session_with_buffers(3, names);
	fill_buffer_handles(buffers, 3);
	CHECK(win_arrange_grid(2, rows, buffers, 3, 1) == KG_WINDOW_LAYOUT_OK);
	for (int i = 0; i < 3; i++) {
		struct editor_window *w = &winlist[i];

		w->cx = i + 1;
		w->cy = i;
		w->rowoff = i + 1;
		w->coloff = i;
		w->rowoff_visual = i + 3;
		w->desired_visual_col = i + 7;
	}
	/* Selected slot 1 has ten text rows before resize and only four after.
	 * Its logical point must stay row 7, column 4 across that shrink. */
	winlist[1].rowoff = 1;
	winlist[1].cy = 6;
	winlist[1].coloff = 1;
	winlist[1].cx = 3;
	win_configuration_save(&configuration);
	CHECK(configuration.valid);
	int saved_filerow = wcur()->rowoff + wcur()->cy;
	int saved_filecol = wcur()->coloff + wcur()->cx;
	for (int i = 0; i < 3; i++) {
		before[i] = win_handle(i);
	}

	kg_event_drain_safe();
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	check_configuration_view_restored(&configuration);
	for (int i = 0; i < 3; i++) {
		CHECK(win_resolve(before[i]) == NULL);
		CHECK(winlist[i].vgeom == NULL);
	}
	(void)get_total_visual_rows(wcur(), bcur());
	CHECK(wcur()->vgeom != NULL);
	struct kg_window_handle first_restore = win_handle(win_current);
	kg_event_drain_safe();
	win_total_rows = 12;
	win_total_cols = 41;
	/* Restore is read-only with respect to its caller-owned value and
	 * reuses it against the terminal's new geometry. */
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == configuration.count);
	for (int i = 0; i < configuration.count; i++) {
		struct kg_window_snapshot *snapshot = &configuration.windows[i];

		CHECK(win_shows_buffer(
		    &winlist[snapshot->slot], snapshot->buffer));
		CHECK(winlist[snapshot->slot].col_group == snapshot->col_group);
	}
	CHECK(win_current == configuration.selected_window.slot);
	CHECK(buf_current == win_buffer_slot(wcur()));
	CHECK(wcur()->rowoff + wcur()->cy == saved_filerow);
	CHECK(wcur()->coloff + wcur()->cx == saved_filecol);
	CHECK(wcur()->cy < wcur()->h);
	CHECK(winlist[0].w + winlist[2].w == 40);
	CHECK(winlist[0].h == 5);
	CHECK(win_resolve(first_restore) == NULL);
	CHECK(wcur()->vgeom == NULL);

	teardown_buffer_session(3, names);
}

static void test_configuration_clamps_after_buffer_shrinks(void)
{
	char *names[1];
	struct kg_window_configuration configuration;

	session_with_buffers(1, names);
	wcur()->rowoff = 5;
	wcur()->cy = 2;
	wcur()->coloff = 2;
	wcur()->cx = 3;
	win_configuration_save(&configuration);
	/* Shrink in memory after the save; restore must not revive a point
	 * beyond the new last row/column. */
	while (bcur()->numrows > 1) {
		editor_del_row(bcur(), bcur()->numrows - 1);
	}
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(wcur()->rowoff + wcur()->cy == 0);
	CHECK(wcur()->coloff + wcur()->cx <= bcur()->row[0].size);

	teardown_buffer_session(1, names);
}

static void test_configuration_clamps_nonselected_visual_offset_after_shrink(
    void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2];
	struct kg_window_configuration configuration;
	int rows[] = { 2 };

	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	/* Slot 0 is deliberately non-selected.  Its visual offset is far beyond
	 * what will remain after its buffer shrinks, and the selected pane
	 * cannot repair it as part of cursor refresh. */
	CHECK(win_arrange_grid(1, rows, buffers, 2, 1) == KG_WINDOW_LAYOUT_OK);
	buflist[0].visual_line_mode = 1;
	winlist[0].rowoff_visual = 90;
	win_configuration_save(&configuration);
	while (buflist[0].numrows > 1) {
		editor_del_row(&buflist[0], buflist[0].numrows - 1);
	}
	/* Slot 0 is normalized first, so this forces that non-selected pane's
	 * total-row query through vgeom's allocation-free scan fallback. */
	vgeom_fail_alloc_after(0);
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_current == 1);
	CHECK(winlist[0].rowoff_visual == 0);
	CHECK(winlist[0].vgeom == NULL);
	CHECK(winlist[1].vgeom == NULL);
	vgeom_fail_alloc_after(-1);

	teardown_buffer_session(2, names);
}

static void test_configuration_canonicalizes_column_labels_for_split(void)
{
	char *names[1];
	struct kg_window_configuration configuration;

	session_with_buffers(1, names);
	win_configuration_save(&configuration);
	configuration.windows[0].col_group = INT_MAX;
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(wcur()->col_group == 0);
	/* An ordinary vertical split computes max_group + 1.  Dense restore
	 * labels keep this defined even for the largest valid public input. */
	win_split_vertical();
	CHECK(win_count == 2);
	CHECK(winlist[0].col_group == 0);
	CHECK(winlist[other_window()].col_group == 1);

	teardown_buffer_session(1, names);
}

static void test_configuration_events_use_old_then_fresh_handles(void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2];
	struct kg_window_configuration configuration;
	struct kg_event events[4];
	struct kg_window_handle old[2];
	int rows[] = { 2 };

	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	CHECK(win_arrange_grid(1, rows, buffers, 2, 1) == KG_WINDOW_LAYOUT_OK);
	win_configuration_save(&configuration);
	old[0] = win_handle(0);
	old[1] = win_handle(1);
	kg_event_drain_safe();

	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(drain_all(events, 4) == 4);
	for (int i = 0; i < 2; i++) {
		CHECK(events[i].kind == KG_EVENT_VIEW_DETACHED);
		CHECK(
		    same_window_handle(events[i].payload.view.window, old[i]));
		CHECK(events[i].payload.view.buffer.id == buffers[i].id);
		CHECK(win_resolve(old[i]) == NULL);
	}
	for (int i = 0; i < 2; i++) {
		struct kg_window_handle fresh = win_handle(i);
		struct kg_event *event = &events[i + 2];

		CHECK(event->kind == KG_EVENT_VIEW_ATTACHED);
		CHECK(event->payload.view.window.id == fresh.id);
		CHECK(event->payload.view.window.id != old[i].id);
		CHECK(win_resolve(event->payload.view.window) == &winlist[i]);
		CHECK(event->payload.view.buffer.id == buffers[i].id);
	}

	teardown_buffer_session(2, names);
}

static void test_configuration_dead_buffer_fallbacks(void)
{
	char *names[4];
	struct kg_buffer_handle buffers[4];
	struct kg_window_configuration configuration;
	int saved_rows[] = { 2 }, other_rows[] = { 2 };

	/* The selected saved buffer dies: first surviving saved window wins. */
	session_with_buffers(4, names);
	fill_buffer_handles(buffers, 4);
	CHECK(win_arrange_grid(1, saved_rows, buffers, 2, 1)
	    == KG_WINDOW_LAYOUT_OK);
	win_configuration_save(&configuration);
	CHECK(win_arrange_grid(1, other_rows, &buffers[2], 2, 0)
	    == KG_WINDOW_LAYOUT_OK);
	CHECK(buf_kill_buffer(buffers[1]));
	kg_event_drain_safe();
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == 1);
	CHECK(win_shows_buffer(wcur(), buffers[0]));
	CHECK(buf_current == buffers[0].slot);
	teardown_buffer_session(4, names);

	/* A dead unselected window is omitted without moving selection. */
	session_with_buffers(4, names);
	fill_buffer_handles(buffers, 4);
	CHECK(win_arrange_grid(1, saved_rows, buffers, 2, 1)
	    == KG_WINDOW_LAYOUT_OK);
	win_configuration_save(&configuration);
	CHECK(win_arrange_grid(1, other_rows, &buffers[2], 2, 0)
	    == KG_WINDOW_LAYOUT_OK);
	CHECK(buf_kill_buffer(buffers[0]));
	kg_event_drain_safe();
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == 1);
	CHECK(win_current == 1);
	CHECK(win_shows_buffer(wcur(), buffers[1]));
	CHECK(buf_current == buffers[1].slot);
	teardown_buffer_session(4, names);
}

static void test_configuration_dead_handle_does_not_follow_slot_reuse(void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2], replacement;
	struct kg_window_configuration configuration;
	int rows[] = { 2 };

	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	CHECK(win_arrange_grid(1, rows, buffers, 2, 0) == KG_WINDOW_LAYOUT_OK);
	win_configuration_save(&configuration);
	CHECK(buf_kill_buffer(buffers[0]));
	replacement = buf_create_named("replacement-in-reused-slot");
	CHECK(buf_resolve(replacement) != NULL);
	CHECK(replacement.slot == buffers[0].slot);
	CHECK(replacement.id != buffers[0].id);
	kg_event_drain_safe();

	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == 1);
	CHECK(win_shows_buffer(wcur(), buffers[1]));
	CHECK(!win_shows_buffer(wcur(), replacement));

	teardown_buffer_session(2, names);
}

static void test_configuration_none_survive_creates_scratch(void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2];
	struct kg_window_configuration configuration;
	struct kg_event events[3];
	int one[] = { 1 };

	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	win_configuration_save(&configuration);
	CHECK(
	    win_arrange_grid(1, one, &buffers[1], 1, 0) == KG_WINDOW_LAYOUT_OK);
	CHECK(buf_kill_buffer(buffers[0]));
	kg_event_drain_safe();
	CHECK(buf_find_open("*scratch*") < 0);
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == 1);
	CHECK(strcmp(bcur()->filename, "*scratch*") == 0);
	CHECK(bcur()->no_file);
	CHECK(drain_all(events, 3) == 3);
	CHECK(events[0].kind == KG_EVENT_BUFFER_OPENED);
	CHECK(events[1].kind == KG_EVENT_VIEW_DETACHED);
	CHECK(events[2].kind == KG_EVENT_VIEW_ATTACHED);

	teardown_buffer_session(2, names);
}

static void test_configuration_none_survive_reuses_existing_scratch(void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2], scratch;
	struct kg_window_configuration configuration;
	struct kg_event events[2];
	int one[] = { 1 };

	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	win_configuration_save(&configuration);
	scratch = buf_create_named("*scratch*");
	CHECK(buf_resolve(scratch) != NULL);
	CHECK(
	    win_arrange_grid(1, one, &buffers[1], 1, 0) == KG_WINDOW_LAYOUT_OK);
	CHECK(buf_kill_buffer(buffers[0]));
	kg_event_drain_safe();
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	CHECK(win_shows_buffer(wcur(), scratch));
	CHECK(drain_all(events, 2) == 2);
	CHECK(events[0].kind == KG_EVENT_VIEW_DETACHED);
	CHECK(events[1].kind == KG_EVENT_VIEW_ATTACHED);

	teardown_buffer_session(2, names);
}

static void test_configuration_scratch_failures_are_atomic(void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2];
	struct kg_window_configuration dead_configuration;
	struct kg_event_reservation all_credits[3];
	struct layout_observation before;
	int one[] = { 1 };

	/* The one fallible pre-commit allocation leaves even the event queue
	 * and old vgeom untouched. */
	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	win_configuration_save(&dead_configuration);
	CHECK(
	    win_arrange_grid(1, one, &buffers[1], 1, 0) == KG_WINDOW_LAYOUT_OK);
	CHECK(buf_kill_buffer(buffers[0]));
	kg_event_drain_safe();
	/* OPEN + old DETACH + new ATTACH exactly fill this logical ring. */
	kg_event_queue_set_capacity_for_test(3);
	wcur()->vgeom = malloc(1);
	before = observe_layout();
	buf_create_named_fail_alloc_once_for_test();
	CHECK(win_configuration_restore(&dead_configuration)
	    == KG_WINDOW_LAYOUT_ALLOCATION_FAILED);
	check_layout_unchanged(&before);
	/* The failed allocation released every pre-reserved credit. */
	CHECK(kg_event_reserve_lifecycle_batch(all_credits, 3));
	for (int i = 0; i < 3; i++) {
		kg_event_release_reservation(&all_credits[i]);
	}
	CHECK(win_configuration_restore(&dead_configuration)
	    == KG_WINDOW_LAYOUT_OK);
	kg_event_queue_init();
	teardown_buffer_session(2, names);

	/* A full buffer table refuses before reserving or detaching anything.
	 */
	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	win_configuration_save(&dead_configuration);
	CHECK(
	    win_arrange_grid(1, one, &buffers[1], 1, 0) == KG_WINDOW_LAYOUT_OK);
	CHECK(buf_kill_buffer(buffers[0]));
	for (int i = 0; i < MAX_BUFFERS && buf_count < MAX_BUFFERS; i++) {
		char name[32];

		snprintf(name, sizeof(name), "fill-%d", i);
		CHECK(buf_resolve(buf_create_named(name)) != NULL);
	}
	CHECK(buf_count == MAX_BUFFERS);
	kg_event_drain_safe();
	wcur()->vgeom = malloc(1);
	before = observe_layout();
	CHECK(win_configuration_restore(&dead_configuration)
	    == KG_WINDOW_LAYOUT_BUFFER_CAPACITY);
	check_layout_unchanged(&before);
	teardown_buffer_session(2, names);
}

static void test_grid_success_minimum_asymmetric_and_live_point(void)
{
	char *names[6];
	struct kg_buffer_handle buffers[6];
	int six_rows[] = { 3, 3 }, asymmetric[] = { 1, 2 };

	session_with_buffers(6, names);
	fill_buffer_handles(buffers, 6);
	/* last_point is deliberately stale.  Detaching the live view must
	 * bank and preserve this current point before the grid attaches it. */
	wcur()->cx = 3;
	wcur()->cy = 2;
	wcur()->rowoff = 1;
	wcur()->coloff = 2;
	bcur()->last_point = (struct kg_point) { 0 };
	win_total_rows = 7;
	win_total_cols = 3;
	CHECK(win_arrange_grid(2, six_rows, buffers, 6, 0)
	    == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == 6);
	CHECK(wcur()->rowoff + wcur()->cy == 3);
	CHECK(wcur()->coloff + wcur()->cx == 5);
	for (int i = 0; i < 6; i++) {
		CHECK(winlist[i].active);
		CHECK(winlist[i].col_group == (i < 3 ? 0 : 1));
		CHECK(winlist[i].h == 1);
		CHECK(winlist[i].w == 1);
	}
	CHECK(win_current == 0 && buf_current == buffers[0].slot);

	win_total_rows = 5;
	CHECK(win_arrange_grid(2, asymmetric, buffers, 3, 2)
	    == KG_WINDOW_LAYOUT_OK);
	CHECK(win_count == 3);
	CHECK(winlist[0].col_group == 0);
	CHECK(winlist[1].col_group == 1);
	CHECK(winlist[2].col_group == 1);
	CHECK(win_current == 2 && buf_current == buffers[2].slot);
	invariants();

	teardown_buffer_session(6, names);
}

static void test_configuration_restore_minimum_geometry(void)
{
	char *names[6];
	struct kg_buffer_handle buffers[6];
	struct kg_window_configuration configuration;
	struct layout_observation before;
	int rows[] = { 3, 3 };

	session_with_buffers(6, names);
	fill_buffer_handles(buffers, 6);
	CHECK(win_arrange_grid(2, rows, buffers, 6, 2) == KG_WINDOW_LAYOUT_OK);
	win_configuration_save(&configuration);
	kg_event_drain_safe();
	win_total_rows = 7;
	win_total_cols = 3;
	CHECK(win_configuration_restore(&configuration) == KG_WINDOW_LAYOUT_OK);
	for (int i = 0; i < 6; i++) {
		CHECK(winlist[i].h == 1);
		CHECK(winlist[i].w == 1);
	}
	kg_event_drain_safe();
	wcur()->vgeom = malloc(1);
	before = observe_layout();

	win_total_rows = 6;
	CHECK(win_configuration_restore(&configuration)
	    == KG_WINDOW_LAYOUT_TOO_SMALL);
	check_layout_unchanged(&before);
	win_total_rows = 7;
	win_total_cols = 2;
	CHECK(win_configuration_restore(&configuration)
	    == KG_WINDOW_LAYOUT_TOO_SMALL);
	check_layout_unchanged(&before);

	teardown_buffer_session(6, names);
}

static void test_grid_failures_leave_everything_unchanged(void)
{
	char *names[4];
	struct kg_buffer_handle buffers[4], duplicate[2];
	struct layout_observation before;
	int two_columns[] = { 1, 1 }, bad_sum[] = { 1, 2 };
	int too_many[] = { MAX_WINDOWS + 1 };

	session_with_buffers(4, names);
	fill_buffer_handles(buffers, 4);
	CHECK(buf_kill_buffer(buffers[3]));
	kg_event_drain_safe();
	wcur()->vgeom = malloc(1);
	before = observe_layout();
	duplicate[0] = duplicate[1] = buffers[1];

	CHECK(win_arrange_grid(2, two_columns, duplicate, 2, 0)
	    == KG_WINDOW_LAYOUT_DUPLICATE_BUFFER);
	check_layout_unchanged(&before);
	CHECK(win_arrange_grid(2, two_columns, &buffers[2], 2, 0)
	    == KG_WINDOW_LAYOUT_BUFFER_GONE);
	check_layout_unchanged(&before);
	CHECK(win_arrange_grid(2, bad_sum, buffers, 2, 0)
	    == KG_WINDOW_LAYOUT_INVALID);
	check_layout_unchanged(&before);
	CHECK(win_arrange_grid(1, too_many, buffers, 1, 0)
	    == KG_WINDOW_LAYOUT_WINDOW_CAPACITY);
	check_layout_unchanged(&before);

	win_total_rows = 2; /* one row/column needs 1 echo + 2 pane rows. */
	CHECK(win_arrange_grid(2, two_columns, buffers, 2, 0)
	    == KG_WINDOW_LAYOUT_TOO_SMALL);
	check_layout_unchanged(&before);
	win_total_rows = 3;
	win_total_cols = 2; /* two one-cell columns also need one separator. */
	CHECK(win_arrange_grid(2, two_columns, buffers, 2, 0)
	    == KG_WINDOW_LAYOUT_TOO_SMALL);
	check_layout_unchanged(&before);
	win_total_cols = 3;
	kg_event_queue_set_capacity_for_test(2);
	CHECK(win_arrange_grid(2, two_columns, buffers, 2, 0)
	    == KG_WINDOW_LAYOUT_EVENT_CAPACITY);
	check_layout_unchanged(&before);
	kg_event_queue_init();

	teardown_buffer_session(4, names);
}

static void test_configuration_rejects_malformed_public_values(void)
{
	char *names[2];
	struct kg_buffer_handle buffers[2];
	struct kg_window_configuration valid, malformed[6];
	struct layout_observation before;
	int rows[] = { 2 };

	session_with_buffers(2, names);
	fill_buffer_handles(buffers, 2);
	CHECK(win_arrange_grid(1, rows, buffers, 2, 0) == KG_WINDOW_LAYOUT_OK);
	win_configuration_save(&valid);
	for (int i = 0; i < 6; i++) {
		malformed[i] = valid;
	}
	malformed[0].windows[0].buffer.slot = MAX_BUFFERS;
	malformed[1].windows[1].slot = malformed[1].windows[0].slot;
	malformed[1].windows[1].window.slot = malformed[1].windows[0].slot;
	malformed[2].windows[1].slot = malformed[2].windows[0].slot;
	malformed[2].windows[1].window = malformed[2].windows[0].window;
	malformed[3].selected_window.id++;
	malformed[4].windows[0].rowoff = -1;
	malformed[5].windows[0].rowoff = INT_MAX;
	malformed[5].windows[0].cy = 1;
	kg_event_drain_safe();
	wcur()->vgeom = malloc(1);
	before = observe_layout();
	for (int i = 0; i < 6; i++) {
		CHECK(win_configuration_restore(&malformed[i])
		    == KG_WINDOW_LAYOUT_INVALID);
		check_layout_unchanged(&before);
	}
	teardown_buffer_session(2, names);
}

#if KG_DEBUG_STATE
#include <signal.h>
#include <sys/wait.h>

/* The seventh invariant, exercised the only way a debug-only abort can be:
 * fork, corrupt a live buffer's mark to name a buffer it does not belong
 * to, and check that the child dies of the invariant rather than of
 * something the corruption happened to break downstream. */
static void test_invariant_seven_fires_on_corrupted_marker_owner(void)
{
	char *names[1];
	pid_t pid;
	int status;

	write_text_file(tmppath("a.txt"), "alpha\n");
	names[0] = strdup(tmppath("a.txt"));
	session(1, names);

	CHECK(test_set_mark(bcur(), 0, 2));
	CHECK(bcur()->mark.marker.id != 0);
	/* The one field a resolved lookup alone would not catch: the marker
	 * still resolves fine, just to a buffer identity that is not its
	 * owner's. */
	bcur()->mark.marker.buffer.id ^= 0xdeadbeefu;

	pid = fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		kg_state_check("test");
		_exit(0);
	} else if (pid > 0) {
		CHECK(waitpid(pid, &status, 0) == pid);
		CHECK(WIFSIGNALED(status));
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
	}

	/* Undo the corruption before the normal teardown path resolves it
	 * for real. */
	bcur()->mark.marker.buffer.id ^= 0xdeadbeefu;
	session_teardown();
	free(names[0]);
}
#endif

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
	RUN(test_undisplay_buffer_drops_the_window_not_the_buffer);
	RUN(test_delete_others_banks_the_views_it_discards);
	RUN(test_autorevert_poll_reverts_only_the_current_buffer);
	RUN(test_deferred_revert_clamps_point_past_new_eof);
	RUN(test_revert_drops_the_region_it_finds);
	RUN(test_revert_clamps_every_window_on_the_buffer);
	RUN(test_kill_buffer_shown_twice);
	RUN(test_kill_shown_in_two_windows_publishes_killing_detach_killed);
	RUN(test_slot_exhaustion_and_reuse);
	RUN(test_last_slot_lifecycle);
	RUN(test_append_to_hidden_buffer_leaves_current_alone);
	RUN(test_append_to_visible_buffer_follows_that_window);
	RUN(test_reflow_sizes);
	RUN(test_marks_belong_to_the_buffer);
	RUN(test_handles_do_not_survive_their_buffer);
	RUN(test_zeroed_handle_names_nothing);
	RUN(test_check_handles_recovers_an_injected_stale_view);
	RUN(test_last_buffer_leaves_no_stale_view);
	RUN(test_special_buffer_reuse_bumps_identity);
	RUN(test_a_user_buffer_is_not_adopted_as_a_special_one);
	RUN(test_current_buffer_is_the_selected_window_s);
	RUN(test_goal_column_belongs_to_the_view);
	RUN(test_window_handle_resolves_until_slot_released_and_reused);
	RUN(test_split_gives_the_new_window_a_distinct_identity);
	RUN(test_open_precedes_attach_when_the_session_starts);
	RUN(test_split_publishes_view_attached_for_the_new_window);
	RUN(test_configuration_round_trip_resize_and_reuse);
	RUN(test_configuration_clamps_after_buffer_shrinks);
	RUN(test_configuration_clamps_nonselected_visual_offset_after_shrink);
	RUN(test_configuration_canonicalizes_column_labels_for_split);
	RUN(test_configuration_events_use_old_then_fresh_handles);
	RUN(test_configuration_dead_buffer_fallbacks);
	RUN(test_configuration_dead_handle_does_not_follow_slot_reuse);
	RUN(test_configuration_none_survive_creates_scratch);
	RUN(test_configuration_none_survive_reuses_existing_scratch);
	RUN(test_configuration_scratch_failures_are_atomic);
	RUN(test_grid_success_minimum_asymmetric_and_live_point);
	RUN(test_configuration_restore_minimum_geometry);
	RUN(test_grid_failures_leave_everything_unchanged);
	RUN(test_configuration_rejects_malformed_public_values);
#if KG_DEBUG_STATE
	RUN(test_invariant_seven_fires_on_corrupted_marker_owner);
#endif

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
	if (system(cmd) != 0) {
		fprintf(stderr, "warning: could not remove %s\n", tmpdir);
	}
	return test_summary();
}
