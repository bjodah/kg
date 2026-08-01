/* test_event.c — unit and reference-model tests for the typed event queue
 *
 * Phase 4 delivers the queue complete and tested through its own API;
 * nothing in src/ calls it yet.  These tests therefore play the part of
 * a producer by hand: they build buffer handles, call the constructors
 * and the queueing/reservation entry points directly, and drain with
 * kg_event_queue_pop() -- exactly the shape the next slice's producers
 * and Phase 5's drain will use. */

#include "../src/bufhandle.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/event.h"
#include "test.h"
#include <stdint.h>
#include <string.h>

static void setup(void)
{
	free_all_rows();
	reset_current_buffer();
	bcur()->active = 1;
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
	kg_event_queue_init();
}

static void teardown(void)
{
	undo_free();
	free_all_rows();
	kg_event_queue_init();
}

static struct kg_buffer_handle mkbuf(int slot, uint64_t id, uint64_t gen)
{
	return (struct kg_buffer_handle) {
		.slot = slot, .id = id, .generation = gen
	};
}

static struct kg_event_buffer_extent mkext(uint64_t old_len, uint64_t new_len)
{
	return (struct kg_event_buffer_extent) {
		.old_total_len = old_len,
		.new_total_len = new_len,
	};
}

/* Drain every remaining event into `out` (capacity `max`), in publication
 * order.  Returns the count. */
static size_t drain_all(struct kg_event *out, size_t max)
{
	size_t n = 0;

	while (n < max && kg_event_queue_pop(&out[n])) {
		n++;
	}
	return n;
}

/* ---- payload constructors ------------------------------------------------ */

static void test_event_payload_constructors(void)
{
	setup();
	struct kg_buffer_handle b = mkbuf(0, 1, 1);

	struct kg_event e1 = kg_event_make_buffer_changed(b, 10, 3, 5, 7);
	CHECK(e1.kind == KG_EVENT_BUFFER_CHANGED);
	CHECK(e1.payload.changed.buffer.id == 1);
	CHECK(e1.payload.changed.begin_byte == 10);
	CHECK(e1.payload.changed.old_len == 3);
	CHECK(e1.payload.changed.new_len == 5);
	CHECK(e1.payload.changed.generation == 7);

	struct kg_event e2 = kg_event_make_buffer_broad(b, mkext(100, 120), 9);
	CHECK(e2.kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(e2.payload.broad.extent.old_total_len == 100);
	CHECK(e2.payload.broad.extent.new_total_len == 120);
	CHECK(e2.payload.broad.generation == 9);

	struct kg_event e3 = kg_event_make_buffer_opened(b);
	CHECK(e3.kind == KG_EVENT_BUFFER_OPENED);
	CHECK(e3.payload.buffer_life.buffer.id == 1);

	struct kg_event e4 = kg_event_make_buffer_killing(b);
	CHECK(e4.kind == KG_EVENT_BUFFER_KILLING);
	CHECK(e4.payload.buffer_life.buffer.id == 1);

	struct kg_event e5 = kg_event_make_buffer_killed(b);
	CHECK(e5.kind == KG_EVENT_BUFFER_KILLED);
	CHECK(e5.payload.buffer_life.buffer.id == 1);

	struct kg_event e6 = kg_event_make_view_attached(2, b);
	CHECK(e6.kind == KG_EVENT_VIEW_ATTACHED);
	CHECK(e6.payload.view.window_slot == 2);
	CHECK(e6.payload.view.buffer.id == 1);

	struct kg_event e7 = kg_event_make_view_detached(3, b);
	CHECK(e7.kind == KG_EVENT_VIEW_DETACHED);
	CHECK(e7.payload.view.window_slot == 3);

	struct kg_event e8 = kg_event_make_before_save(b);
	CHECK(e8.kind == KG_EVENT_BEFORE_SAVE);
	CHECK(e8.payload.before_save.buffer.id == 1);

	struct kg_event e9 = kg_event_make_after_save(b, true);
	CHECK(e9.kind == KG_EVENT_AFTER_SAVE);
	CHECK(e9.payload.after_save.success == true);
	struct kg_event e10 = kg_event_make_after_save(b, false);
	CHECK(e10.payload.after_save.success == false);

	struct kg_event e11 = kg_event_make_mode_changed(b, "Python");
	CHECK(e11.kind == KG_EVENT_MODE_CHANGED);
	CHECK(strcmp(e11.payload.mode_changed.name, "Python") == 0);

	/* NULL name leaves an empty string, not garbage. */
	struct kg_event e12 = kg_event_make_mode_changed(b, NULL);
	CHECK(e12.payload.mode_changed.name[0] == '\0');

	/* A name at or past the bound is truncated, not overrun. */
	char long_name[64];
	memset(long_name, 'x', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	struct kg_event e13 = kg_event_make_mode_changed(b, long_name);
	CHECK(strlen(e13.payload.mode_changed.name)
	    == KG_EVENT_MODE_NAME_MAX - 1);

	teardown();
}

/* ---- refused/no-op edits queue nothing ------------------------------------
 *
 * The queue itself cannot see a refusal: a producer simply does not call
 * kg_event_queue_text_change() when kg_buffer_replace() reports no
 * change.  This test documents and exercises that calling convention
 * against the real edit gateway. */
static void test_event_refused_and_noop_edits_queue_nothing(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	struct kg_edit noop = kg_edit_user(bcur(), 0, 5, "hello", 5);
	struct kg_edit_result r = { 0 };
	CHECK(kg_buffer_replace(&noop, &r) == 1);
	CHECK(r.before_generation == r.after_generation);
	CHECK(kg_event_queue_pop(NULL) == false);

	struct kg_edit bad = kg_edit_user(bcur(), 100, 200, "x", 1);
	struct kg_edit_result r2 = { 0 };
	CHECK(kg_buffer_replace(&bad, &r2) == 0);
	CHECK(r2.before_generation == r2.after_generation);
	CHECK(kg_event_queue_pop(NULL) == false);

	bcur()->readonly = 1;
	struct kg_edit ro = kg_edit_user(bcur(), 0, 5, "world", 5);
	struct kg_edit_result r3 = { 0 };
	CHECK(kg_buffer_replace(&ro, &r3) == 0);
	CHECK(r3.before_generation == r3.after_generation);
	CHECK(kg_event_queue_pop(NULL) == false);
	bcur()->readonly = 0;

	teardown();
}

/* ---- a successful edit produces exactly one exact change event -----------
 */

static void test_event_successful_edit_produces_one_exact_event(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	uint64_t before_gen = bcur()->content_generation;

	struct kg_edit e = kg_edit_user(bcur(), 1, 3, "ELLO", 4);
	struct kg_edit_result r = { 0 };
	CHECK(kg_buffer_replace(&e, &r) == 1);
	CHECK(r.after_generation == before_gen + 1);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_CHANGED);
	CHECK(ev.payload.changed.buffer.slot == 0);
	CHECK(ev.payload.changed.begin_byte == 1);
	CHECK(ev.payload.changed.old_len == 2);
	CHECK(ev.payload.changed.new_len == 4);
	CHECK(ev.payload.changed.generation == r.after_generation);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

/* ---- kg_buffer_adopt_rows produces exactly one broad event ---------------
 */

static void test_event_adopt_rows_produces_one_broad_event(void)
{
	setup();
	editor_insert_row(bcur(), 0, "one", 3);
	editor_insert_row(bcur(), 1, "two", 3);
	size_t old_total = buffer_byte_length(bcur());

	erow *rows = NULL;
	int numrows = 0, row_capacity = 0;
	struct editor_buffer staged = { .active = 1 };
	editor_insert_row(&staged, 0, "abcde", 5);
	rows = staged.row;
	numrows = staged.numrows;
	row_capacity = staged.row_capacity;

	kg_buffer_adopt_rows(bcur(), &rows, &numrows, &row_capacity);
	size_t new_total = buffer_byte_length(bcur());

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(ev.payload.broad.buffer.slot == 0);
	CHECK(ev.payload.broad.extent.old_total_len == old_total);
	CHECK(ev.payload.broad.extent.new_total_len == new_total);
	CHECK(ev.payload.broad.generation == bcur()->content_generation);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

/* ---- exact ordering -------------------------------------------------------
 */

static void test_event_exact_ordering(void)
{
	setup();
	struct kg_buffer_handle b0 = mkbuf(0, 1, 1);
	struct kg_buffer_handle b1 = mkbuf(1, 2, 1);
	struct kg_buffer_handle b2 = mkbuf(2, 3, 1);

	CHECK(
	    kg_event_queue_text_change(b0, 0, 0, 1, mkext(0, 1), 1, CMD_ID_NONE)
	    == KG_EVENT_QUEUED_EXACT);
	CHECK(kg_event_queue_broad_change(b1, mkext(0, 50), 1)
	    == KG_EVENT_QUEUED_EXACT);

	struct kg_event_reservation res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	CHECK(kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(b2))
	    == KG_EVENT_QUEUED);

	CHECK(
	    kg_event_queue_text_change(b0, 5, 0, 2, mkext(1, 3), 2, CMD_ID_NONE)
	    == KG_EVENT_QUEUED_EXACT);

	struct kg_event out[8];
	size_t n = drain_all(out, 8);
	CHECK(n == 4);
	CHECK(out[0].kind == KG_EVENT_BUFFER_CHANGED);
	CHECK(out[0].payload.changed.buffer.id == 1);
	CHECK(out[1].kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(out[1].payload.broad.buffer.id == 2);
	CHECK(out[2].kind == KG_EVENT_BUFFER_OPENED);
	CHECK(out[2].payload.buffer_life.buffer.id == 3);
	CHECK(out[3].kind == KG_EVENT_BUFFER_CHANGED);
	CHECK(out[3].payload.changed.begin_byte == 5);
	CHECK(out[0].seq < out[1].seq);
	CHECK(out[1].seq < out[2].seq);
	CHECK(out[2].seq < out[3].seq);

	teardown();
}

/* ---- coalescing: the contained case merges exactly ------------------------
 */

static void test_event_coalescing_merges_contained_edit(void)
{
	setup();
	struct kg_buffer_handle b = mkbuf(0, 1, 1);
	command_id token = 7;

	/* Two successive inserts growing from one point, the
	 * self-insert-command shape: [10,10)->1 byte, then the next byte
	 * lands exactly at the new right edge. */
	CHECK(kg_event_queue_text_change(b, 10, 0, 1, mkext(0, 1), 1, token)
	    == KG_EVENT_QUEUED_EXACT);
	CHECK(kg_event_queue_text_change(b, 11, 0, 1, mkext(1, 2), 2, token)
	    == KG_EVENT_QUEUED_EXACT);

	struct kg_event out[4];
	CHECK(drain_all(out, 4) == 1);
	CHECK(out[0].payload.changed.begin_byte == 10);
	CHECK(out[0].payload.changed.old_len == 0);
	CHECK(out[0].payload.changed.new_len == 2);
	CHECK(out[0].payload.changed.generation == 2);

	/* A third edit landing inside the merged span (a backspace right
	 * after typing) merges again. */
	kg_event_queue_init();
	CHECK(kg_event_queue_text_change(b, 10, 0, 5, mkext(0, 5), 1, token)
	    == KG_EVENT_QUEUED_EXACT);
	CHECK(kg_event_queue_text_change(b, 12, 2, 0, mkext(5, 3), 2, token)
	    == KG_EVENT_QUEUED_EXACT);
	CHECK(drain_all(out, 4) == 1);
	CHECK(out[0].payload.changed.begin_byte == 10);
	CHECK(out[0].payload.changed.old_len == 0);
	CHECK(out[0].payload.changed.new_len == 3);
	CHECK(out[0].payload.changed.generation == 2);

	teardown();
}

static void test_event_coalescing_refuses_ineligible_pairs(void)
{
	setup();
	struct kg_buffer_handle b0 = mkbuf(0, 1, 1);
	struct kg_buffer_handle b1 = mkbuf(1, 2, 1);
	command_id token = 3;
	struct kg_event out[4];

	/* Different buffer: no merge. */
	kg_event_queue_init();
	kg_event_queue_text_change(b0, 10, 0, 1, mkext(0, 1), 1, token);
	kg_event_queue_text_change(b1, 10, 0, 1, mkext(0, 1), 2, token);
	CHECK(drain_all(out, 4) == 2);

	/* Different token: no merge. */
	kg_event_queue_init();
	kg_event_queue_text_change(b0, 10, 0, 1, mkext(0, 1), 1, token);
	kg_event_queue_text_change(b0, 11, 0, 1, mkext(1, 2), 2, token + 1);
	CHECK(drain_all(out, 4) == 2);

	/* CMD_ID_NONE never merges, even against itself. */
	kg_event_queue_init();
	kg_event_queue_text_change(b0, 10, 0, 1, mkext(0, 1), 1, CMD_ID_NONE);
	kg_event_queue_text_change(b0, 11, 0, 1, mkext(1, 2), 2, CMD_ID_NONE);
	CHECK(drain_all(out, 4) == 2);

	/* Generation gap: no merge. */
	kg_event_queue_init();
	kg_event_queue_text_change(b0, 10, 0, 1, mkext(0, 1), 1, token);
	kg_event_queue_text_change(b0, 11, 0, 1, mkext(1, 2), 3, token);
	CHECK(drain_all(out, 4) == 2);

	/* Not contained: the second edit starts before the first's span. */
	kg_event_queue_init();
	kg_event_queue_text_change(b0, 10, 0, 1, mkext(0, 1), 1, token);
	kg_event_queue_text_change(b0, 5, 0, 1, mkext(1, 2), 2, token);
	CHECK(drain_all(out, 4) == 2);

	/* A broad change ends the run: a following text change does not
	 * reach back through it. */
	kg_event_queue_init();
	kg_event_queue_text_change(b0, 10, 0, 1, mkext(0, 1), 1, token);
	kg_event_queue_broad_change(b0, mkext(1, 40), 2);
	CHECK(drain_all(out, 4) == 2);

	teardown();
}

/* ---- arithmetic boundaries: no wrap ---------------------------------------
 */

static void test_event_arithmetic_boundaries_do_not_wrap(void)
{
	setup();
	struct kg_buffer_handle b = mkbuf(0, 1, 1);
	command_id token = 1;
	struct kg_event out[4];

	/* begin + old_len would overflow uint64_t on the second edit: must
	 * not merge (and must not corrupt the first entry). */
	kg_event_queue_init();
	kg_event_queue_text_change(
	    b, UINT64_MAX - 2, 0, 2, mkext(0, 2), 1, token);
	kg_event_queue_text_change(
	    b, UINT64_MAX - 1, 5, 1, mkext(2, 0), 2, token);
	CHECK(drain_all(out, 4) == 2);
	CHECK(out[0].payload.changed.begin_byte == UINT64_MAX - 2);
	CHECK(out[0].payload.changed.new_len == 2);
	CHECK(out[1].payload.changed.begin_byte == UINT64_MAX - 1);
	CHECK(out[1].payload.changed.old_len == 5);

	/* Generation already at its type's limit: incrementing it to test
	 * adjacency would itself wrap, so this must refuse rather than
	 * treat a wrapped 0 as "adjacent". */
	kg_event_queue_init();
	kg_event_queue_text_change(b, 10, 0, 1, mkext(0, 1), UINT64_MAX, token);
	kg_event_queue_text_change(b, 11, 0, 1, mkext(1, 2), 0, token);
	CHECK(drain_all(out, 4) == 2);
	CHECK(out[0].payload.changed.generation == UINT64_MAX);
	CHECK(out[1].payload.changed.generation == 0);

	/* prev_begin + prev_new_len would overflow: must not merge. */
	kg_event_queue_init();
	kg_event_queue_text_change(
	    b, UINT64_MAX - 1, 0, 5, mkext(0, 5), 1, token);
	kg_event_queue_text_change(
	    b, UINT64_MAX - 1, 0, 1, mkext(5, 6), 2, token);
	CHECK(drain_all(out, 4) == 2);

	teardown();
}

/* ---- ring wraparound -------------------------------------------------------
 */

static void test_event_ring_wraparound(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(3);

	struct kg_buffer_handle ba = mkbuf(0, 1, 1);
	struct kg_buffer_handle bb = mkbuf(1, 2, 1);
	struct kg_buffer_handle bc = mkbuf(2, 3, 1);
	struct kg_buffer_handle bd = mkbuf(3, 4, 1);
	struct kg_event_reservation res;
	struct kg_event ev;

	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(ba));
	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(bb));

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.payload.buffer_life.buffer.id == 1);

	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(bc));
	res = kg_event_reserve_lifecycle();
	CHECK(res.valid); /* physical index wraps to 0 here */
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(bd));

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.payload.buffer_life.buffer.id == 2);
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.payload.buffer_life.buffer.id == 3);
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.payload.buffer_life.buffer.id == 4);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

/* ---- overflow collapse -----------------------------------------------------
 */

static void test_event_overflow_collapse_one_buffer(void)
{
	setup();
	/* capacity == KG_EVENT_LIFECYCLE_RESERVE: the droppable share is
	 * zero, so every change collapses immediately. */
	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);
	struct kg_buffer_handle b = mkbuf(0, 1, 1);

	CHECK(kg_event_queue_text_change(
		  b, 0, 0, 10, mkext(0, 10), 1, CMD_ID_NONE)
	    == KG_EVENT_QUEUED_BROAD);
	CHECK(kg_event_queue_text_change(
		  b, 5, 0, 15, mkext(10, 25), 2, CMD_ID_NONE)
	    == KG_EVENT_QUEUED_BROAD);
	CHECK(kg_event_queue_text_change(
		  b, 0, 5, 0, mkext(25, 20), 3, CMD_ID_NONE)
	    == KG_EVENT_QUEUED_BROAD);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(ev.payload.broad.extent.old_total_len == 0);
	CHECK(ev.payload.broad.extent.new_total_len == 20);
	CHECK(ev.payload.broad.generation == 3);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

static void test_event_overflow_collapse_several_buffers(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);
	struct kg_buffer_handle b0 = mkbuf(0, 1, 1);
	struct kg_buffer_handle b1 = mkbuf(1, 2, 1);

	kg_event_queue_text_change(b0, 0, 0, 10, mkext(0, 10), 1, CMD_ID_NONE);
	kg_event_queue_text_change(
	    b1, 0, 0, 100, mkext(0, 100), 1, CMD_ID_NONE);
	kg_event_queue_text_change(b0, 0, 0, 5, mkext(10, 15), 2, CMD_ID_NONE);
	kg_event_queue_text_change(
	    b1, 0, 50, 0, mkext(100, 50), 2, CMD_ID_NONE);

	struct kg_event out[4];
	size_t n = drain_all(out, 4);
	CHECK(n == 2);
	for (size_t i = 0; i < n; i++) {
		CHECK(out[i].kind == KG_EVENT_BUFFER_BROAD_CHANGE);
		if (out[i].payload.broad.buffer.id == 1) {
			CHECK(out[i].payload.broad.extent.old_total_len == 0);
			CHECK(out[i].payload.broad.extent.new_total_len == 15);
			CHECK(out[i].payload.broad.generation == 2);
		} else {
			CHECK(out[i].payload.broad.buffer.id == 2);
			CHECK(out[i].payload.broad.extent.old_total_len == 0);
			CHECK(out[i].payload.broad.extent.new_total_len == 50);
			CHECK(out[i].payload.broad.generation == 2);
		}
	}

	teardown();
}

/* ---- lifecycle reservation -------------------------------------------------
 */

static void test_event_lifecycle_reservation_failure_and_release(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(2);

	struct kg_event_reservation r1 = kg_event_reserve_lifecycle();
	struct kg_event_reservation r2 = kg_event_reserve_lifecycle();
	CHECK(r1.valid);
	CHECK(r2.valid);

	struct kg_event_reservation r3 = kg_event_reserve_lifecycle();
	CHECK(!r3.valid);

	kg_event_release_reservation(&r1);
	CHECK(!r1.valid);
	struct kg_event_reservation r4 = kg_event_reserve_lifecycle();
	CHECK(r4.valid);

	/* A double release is a no-op, not an extra credit. */
	kg_event_release_reservation(&r1);
	struct kg_event_reservation r5 = kg_event_reserve_lifecycle();
	CHECK(!r5.valid);

	teardown();
}

static void test_event_publish_requires_a_live_reservation(void)
{
	setup();
	struct kg_buffer_handle b = mkbuf(0, 1, 1);

	struct kg_event_reservation none = { 0 };
	CHECK(kg_event_publish_lifecycle(&none, kg_event_make_buffer_opened(b))
	    == KG_EVENT_REFUSED);
	CHECK(kg_event_queue_pop(NULL) == false);

	struct kg_event_reservation res = kg_event_reserve_lifecycle();
	CHECK(kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(b))
	    == KG_EVENT_QUEUED);
	/* Spent: publishing again with the same reservation is refused and
	 * queues a second time nothing. */
	CHECK(kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(b))
	    == KG_EVENT_REFUSED);

	struct kg_event out[4];
	CHECK(drain_all(out, 4) == 1);

	teardown();
}

static void test_event_lifecycle_round_trip_all_kinds(void)
{
	setup();
	struct kg_buffer_handle b = mkbuf(0, 1, 1);
	struct kg_event_reservation res;

	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_opened(b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_killing(b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_killed(b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_view_attached(0, b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_view_detached(0, b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_before_save(b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_after_save(b, true));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(&res, kg_event_make_mode_changed(b, "C"));

	enum kg_event_kind expect[] = {
		KG_EVENT_BUFFER_OPENED,
		KG_EVENT_BUFFER_KILLING,
		KG_EVENT_BUFFER_KILLED,
		KG_EVENT_VIEW_ATTACHED,
		KG_EVENT_VIEW_DETACHED,
		KG_EVENT_BEFORE_SAVE,
		KG_EVENT_AFTER_SAVE,
		KG_EVENT_MODE_CHANGED,
	};
	struct kg_event out[8];
	CHECK(drain_all(out, 8) == 8);
	for (size_t i = 0; i < 8; i++) {
		CHECK(out[i].kind == expect[i]);
	}
	CHECK(out[6].payload.after_save.success == true);
	CHECK(strcmp(out[7].payload.mode_changed.name, "C") == 0);

	teardown();
}

/* ---- kill before drain ------------------------------------------------------
 */

static void test_event_kill_before_drain(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);
	struct kg_buffer_handle b = mkbuf(0, 1, 1);

	kg_event_queue_text_change(b, 0, 0, 10, mkext(0, 10), 1, CMD_ID_NONE);
	kg_event_queue_text_change(b, 0, 0, 5, mkext(10, 15), 2, CMD_ID_NONE);

	struct kg_event_reservation res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	CHECK(kg_event_publish_lifecycle(&res, kg_event_make_buffer_killed(b))
	    == KG_EVENT_QUEUED);

	struct kg_event out[4];
	CHECK(drain_all(out, 4) == 2);
	CHECK(out[0].kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(out[0].payload.broad.extent.new_total_len == 15);
	CHECK(out[1].kind == KG_EVENT_BUFFER_KILLED);
	CHECK(out[0].seq < out[1].seq);

	teardown();
}

/* ---- slot reuse -------------------------------------------------------------
 *
 * A slot whose overflow entry belongs to a dead identity is not merged
 * into by whoever reuses the slot: the new buffer starts a fresh summary
 * rather than inheriting the old one's lengths.  (This module does not
 * itself track buffer liveness, so a kill immediately followed by reuse
 * with zero drains in between evicts the dead buffer's still-undrained
 * summary rather than preserving it -- an accepted precision limit for a
 * sequence Phase 5's safe-point drains, run after every command, make
 * unreachable in the wired editor.) */

static void test_event_slot_reuse_starts_a_fresh_summary(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);
	struct kg_buffer_handle dead = mkbuf(0, 1, 1);
	struct kg_buffer_handle reused = mkbuf(0, 2, 2);

	kg_event_queue_text_change(
	    dead, 0, 0, 10, mkext(0, 10), 1, CMD_ID_NONE);
	kg_event_queue_text_change(
	    reused, 0, 0, 10, mkext(500, 510), 1, CMD_ID_NONE);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_BROAD_CHANGE);
	CHECK(ev.payload.broad.buffer.id == 2);
	CHECK(ev.payload.broad.extent.old_total_len == 500);
	CHECK(ev.payload.broad.extent.new_total_len == 510);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

/* ---- reference model: every mutation is exact or covered by a broad
 * event -------------------------------------------------------------------- */

static uint32_t rng_state = 0xC0FFEE;
static uint32_t rand_u32(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

#define MODEL_BUFFERS 3

static void test_event_reference_model(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(6);

	struct kg_buffer_handle bufs[MODEL_BUFFERS];
	uint64_t model_total[MODEL_BUFFERS] = { 0 };
	uint64_t model_gen[MODEL_BUFFERS] = { 0 };
	for (int i = 0; i < MODEL_BUFFERS; i++) {
		bufs[i] = mkbuf(i, (uint64_t)(i + 1), 1);
		model_total[i] = 1000;
	}

	for (int step = 0; step < 300; step++) {
		int buf = (int)(rand_u32() % MODEL_BUFFERS);
		uint64_t old_span = rand_u32() % 6;
		uint64_t new_span = rand_u32() % 6;
		if (old_span > model_total[buf]) {
			old_span = model_total[buf];
		}
		uint64_t before = model_total[buf];
		uint64_t after = before - old_span + new_span;
		model_gen[buf]++;
		model_total[buf] = after;
		kg_event_queue_text_change(bufs[buf], 0, old_span, new_span,
		    mkext(before, after), model_gen[buf], CMD_ID_NONE);
	}

	uint64_t replay_total[MODEL_BUFFERS] = { 0 };
	uint64_t replay_gen[MODEL_BUFFERS] = { 0 };
	for (int i = 0; i < MODEL_BUFFERS; i++) {
		replay_total[i] = 1000;
	}

	struct kg_event ev;
	int delivered = 0;
	while (kg_event_queue_pop(&ev)) {
		delivered++;
		int slot = -1;
		if (ev.kind == KG_EVENT_BUFFER_CHANGED) {
			slot = ev.payload.changed.buffer.slot;
			replay_total[slot] = replay_total[slot]
			    - ev.payload.changed.old_len
			    + ev.payload.changed.new_len;
			replay_gen[slot] = ev.payload.changed.generation;
		} else if (ev.kind == KG_EVENT_BUFFER_BROAD_CHANGE) {
			slot = ev.payload.broad.buffer.slot;
			CHECKF(ev.payload.broad.extent.old_total_len
				== replay_total[slot],
			    "buf=%d expected_old=%llu got_old=%llu", slot,
			    (unsigned long long)replay_total[slot],
			    (unsigned long long)
				ev.payload.broad.extent.old_total_len);
			replay_total[slot]
			    = ev.payload.broad.extent.new_total_len;
			replay_gen[slot] = ev.payload.broad.generation;
		}
	}
	CHECK(delivered > 0);

	for (int i = 0; i < MODEL_BUFFERS; i++) {
		CHECKF(replay_total[i] == model_total[i],
		    "buf=%d replay_total=%llu model_total=%llu", i,
		    (unsigned long long)replay_total[i],
		    (unsigned long long)model_total[i]);
		CHECKF(replay_gen[i] == model_gen[i],
		    "buf=%d replay_gen=%llu model_gen=%llu", i,
		    (unsigned long long)replay_gen[i],
		    (unsigned long long)model_gen[i]);
	}

	teardown();
}

int main(void)
{
	RUN(test_event_payload_constructors);
	RUN(test_event_refused_and_noop_edits_queue_nothing);
	RUN(test_event_successful_edit_produces_one_exact_event);
	RUN(test_event_adopt_rows_produces_one_broad_event);
	RUN(test_event_exact_ordering);
	RUN(test_event_coalescing_merges_contained_edit);
	RUN(test_event_coalescing_refuses_ineligible_pairs);
	RUN(test_event_arithmetic_boundaries_do_not_wrap);
	RUN(test_event_ring_wraparound);
	RUN(test_event_overflow_collapse_one_buffer);
	RUN(test_event_overflow_collapse_several_buffers);
	RUN(test_event_lifecycle_reservation_failure_and_release);
	RUN(test_event_publish_requires_a_live_reservation);
	RUN(test_event_lifecycle_round_trip_all_kinds);
	RUN(test_event_kill_before_drain);
	RUN(test_event_slot_reuse_starts_a_fresh_summary);
	RUN(test_event_reference_model);
	return tests_failed ? 1 : 0;
}
