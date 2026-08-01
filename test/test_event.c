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
#include "../src/syntax.h"
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
	/* Mint an identity the way reset_current_buffer() mints one for
	 * bcur(): this suite pokes `.active` directly rather than going
	 * through win_claim_slot(), so nothing else would. */
	wcur()->id = wcur()->id ? wcur()->id : 1;
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

static struct kg_window_handle mkwin(int slot, uint64_t id, uint64_t gen)
{
	return (struct kg_window_handle) {
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

	struct kg_event e6 = kg_event_make_view_attached(mkwin(2, 20, 1), b);
	CHECK(e6.kind == KG_EVENT_VIEW_ATTACHED);
	CHECK(e6.payload.view.window.slot == 2);
	CHECK(e6.payload.view.window.id == 20);
	CHECK(e6.payload.view.buffer.id == 1);

	struct kg_event e7 = kg_event_make_view_detached(mkwin(3, 30, 1), b);
	CHECK(e7.kind == KG_EVENT_VIEW_DETACHED);
	CHECK(e7.payload.view.window.slot == 3);
	CHECK(e7.payload.view.window.id == 30);

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
	kg_event_publish_lifecycle(
	    &res, kg_event_make_view_attached(mkwin(0, 1, 1), b));
	res = kg_event_reserve_lifecycle();
	kg_event_publish_lifecycle(
	    &res, kg_event_make_view_detached(mkwin(0, 1, 1), b));
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

/* ---- lifecycle producers: buffer open/kill, view attach/detach, save,
 * mode changed (bufmgr.c, fileio.c, syntax.c) ------------------------------
 *
 * Unlike the tests above, which play producer by hand, these call the real
 * wired producers linked in by EXTRA_event (bufmgr.o, fileio.o, syntax.o --
 * see the Makefile).  Slot 0 is setup()'s buffer and stays put; a test that
 * activates another slot or moves buf_current restores both before
 * teardown() so later tests in this binary see the same starting state
 * setup() always has. win_count stays 0 in this harness (stubs_win.c), so
 * buf_select() never touches a window's view -- tests that care about
 * attach/detach call buf_attach_view()/buf_detach_view() directly. */

static void test_producer_open_queues_resolvable_handle(void)
{
	setup();
	CHECK(!buflist[1].active);

	CHECK(buf_select(1) == 1);
	CHECK(buflist[1].active);
	CHECK(buf_current == 1);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_OPENED);
	CHECK(buf_resolve(ev.payload.buffer_life.buffer) == &buflist[1]);
	CHECK(kg_event_queue_pop(NULL) == false);

	buf_select(0);
	memset(&buflist[1], 0, sizeof(buflist[1]));
	teardown();
}

static void test_producer_kill_events_killing_resolves_killed_does_not(void)
{
	setup();
	int prev_count = buf_count;
	buf_count = 2; /* pretend a second buffer so buf_kill() does not
			* decide the session is over. */

	/* setup() marks window 0 active but never points it at a buffer
	 * (that is normally buf_select()'s job under win_count > 0, which
	 * this harness keeps at 0); attach it for real first so buf_kill()'s
	 * "reattach any window left showing nothing" pass has nothing to do
	 * and this test sees only the two events it is about. */
	buf_attach_view(&winlist[0], 0);
	CHECK(kg_event_queue_pop(NULL));

	CHECK(buf_select(1) == 1);
	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_OPENED);
	struct kg_buffer_handle h = ev.payload.buffer_life.buffer;
	CHECK(buf_resolve(h) == &buflist[1]);

	buf_kill(0);
	CHECK(buf_current == 0);
	CHECK(!buflist[1].active);

	/* KILLING carries the identity that resolved right up to this call;
	 * KILLED carries the same identity once it no longer does. */
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_KILLING);
	CHECK(ev.payload.buffer_life.buffer.slot == h.slot);
	CHECK(ev.payload.buffer_life.buffer.id == h.id);

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BUFFER_KILLED);
	CHECK(ev.payload.buffer_life.buffer.id == h.id);
	CHECK(!buf_resolve(ev.payload.buffer_life.buffer));

	CHECK(kg_event_queue_pop(NULL) == false);

	buf_count = prev_count;
	teardown();
}

static void test_producer_attach_detach_carry_window_and_buffer(void)
{
	setup();
	struct kg_buffer_handle h0 = buf_handle(0);
	struct kg_window_handle w0;

	winlist[0].buf = (struct kg_buffer_handle) { 0, 0, 0 };

	buf_attach_view(&winlist[0], 0);
	CHECK(winlist[0].buf.id == h0.id);
	/* Captured after the attach, the same way h0 is: this test's stub
	 * window never runs through win_claim_slot(), so its identity is
	 * whatever the environment leaves it -- the event's payload is
	 * checked against that same value, not against a hardcoded one. */
	w0 = win_handle_of(&winlist[0]);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_VIEW_ATTACHED);
	CHECK(ev.payload.view.window.slot == 0);
	CHECK(ev.payload.view.window.id == w0.id);
	CHECK(win_resolve(ev.payload.view.window) == &winlist[0]);
	CHECK(ev.payload.view.buffer.id == h0.id);
	CHECK(kg_event_queue_pop(NULL) == false);

	buf_detach_view(&winlist[0]);
	CHECK(winlist[0].buf.id == 0);

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_VIEW_DETACHED);
	CHECK(ev.payload.view.window.slot == 0);
	CHECK(ev.payload.view.window.id == w0.id);
	CHECK(ev.payload.view.buffer.id == h0.id);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

static void test_producer_failed_save_produces_before_and_unsuccessful_after(
    void)
{
	setup();
	bcur()->filename = strdup("/nonexistent-kg-test-dir/testfile.txt");
	/* Snapshot the (absent) destination as "accepted" so the save does
	 * not stop at a confirm prompt first -- editor_confirm_yn() always
	 * answers "no" in this harness (stubs_buffer.c), which would abort
	 * the save before it ever reached the write this test is about. */
	file_snapshot_path(bcur()->filename, &bcur()->disk);
	editor_insert_row(bcur(), 0, "hi", 2);

	CHECK(editor_save(0) == 1);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_BEFORE_SAVE);
	CHECK(buf_resolve(ev.payload.before_save.buffer) == bcur());

	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_AFTER_SAVE);
	CHECK(ev.payload.after_save.success == false);
	CHECK(buf_resolve(ev.payload.after_save.buffer) == bcur());

	CHECK(kg_event_queue_pop(NULL) == false);

	free(bcur()->filename);
	bcur()->filename = NULL;
	teardown();
}

static void test_producer_mode_changed_carries_name(void)
{
	setup();
	struct editor_syntax syn = { .name = "Python" };

	editor_set_syntax(bcur(), &syn);
	CHECK(bcur()->syntax == &syn);

	struct kg_event ev;
	CHECK(kg_event_queue_pop(&ev));
	CHECK(ev.kind == KG_EVENT_MODE_CHANGED);
	CHECK(buf_resolve(ev.payload.mode_changed.buffer) == bcur());
	CHECK(strcmp(ev.payload.mode_changed.name, "Python") == 0);
	CHECK(kg_event_queue_pop(NULL) == false);

	teardown();
}

/* ---- reservation failure refuses the transition, old state intact ------- */

static void test_producer_reservation_failure_refuses_open(void)
{
	setup();
	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);

	struct kg_event_reservation held[KG_EVENT_LIFECYCLE_RESERVE];
	for (int i = 0; i < KG_EVENT_LIFECYCLE_RESERVE; i++) {
		held[i] = kg_event_reserve_lifecycle();
		CHECK(held[i].valid);
	}

	int prev_current = buf_current;
	CHECK(!buflist[1].active);

	CHECK(buf_select(1) == 0);
	CHECK(!buflist[1].active);
	CHECK(buf_current == prev_current);

	teardown();
}

static void test_producer_reservation_failure_refuses_kill(void)
{
	setup();
	int prev_count = buf_count;
	buf_count = 2;
	CHECK(buf_select(1) == 1);
	CHECK(kg_event_queue_pop(NULL)); /* drain the open event */

	kg_event_queue_set_capacity_for_test(KG_EVENT_LIFECYCLE_RESERVE);
	struct kg_event_reservation held[KG_EVENT_LIFECYCLE_RESERVE];
	for (int i = 0; i < KG_EVENT_LIFECYCLE_RESERVE; i++) {
		held[i] = kg_event_reserve_lifecycle();
		CHECK(held[i].valid);
	}

	buf_kill(0);
	/* Refused before anything about the buffer changed: still active,
	 * still current, nothing published. */
	CHECK(buflist[1].active);
	CHECK(buf_current == 1);
	CHECK(kg_event_queue_pop(NULL) == false);

	buf_select(0);
	memset(&buflist[1], 0, sizeof(buflist[1]));
	buf_count = prev_count;
	teardown();
}

/* ---- Phase 5: subscriber registry and safe-point delivery --------------
 *
 * These build on the same setup()/teardown() as the rest of this file;
 * kg_event_queue_init() (called by both) also resets the subscriber
 * registry, drain depth and error count, so no test here leaks a
 * subscription or a stale count into the next one. */

/* Reserve-and-publish a KG_EVENT_MODE_CHANGED for `h` -- the one lifecycle
 * kind most of these tests use as "some event", since its payload needs
 * nothing beyond a handle. */
static void publish_mode_changed(struct kg_buffer_handle h)
{
	struct kg_event_reservation res = kg_event_reserve_lifecycle();

	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_mode_changed(h, "C"));
}

/* Appends `tag` (from ctx) to a shared log and returns CONTINUE -- the
 * simplest possible subscriber, used wherever a test only cares that a
 * callback ran, and in what order. */
struct log_ctx {
	int *log;
	size_t *count;
	int tag;
};

static enum kg_event_cb_status log_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	struct log_ctx *ctx = ctx_;

	(void)ev;
	(void)res;
	ctx->log[(*ctx->count)++] = ctx->tag;
	return KG_EVENT_CB_CONTINUE;
}

static enum kg_event_cb_status noop_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	(void)ev;
	(void)res;
	(void)ctx_;
	return KG_EVENT_CB_CONTINUE;
}

static enum kg_event_cb_status error_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	(void)ev;
	(void)res;
	(void)ctx_;
	return KG_EVENT_CB_ERROR;
}

static void test_drain_dispatches_subscribers_in_registration_order(void)
{
	setup();
	int log[3];
	size_t count = 0;
	struct log_ctx a = { log, &count, 1 };
	struct log_ctx b = { log, &count, 2 };
	struct log_ctx c = { log, &count, 3 };
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct kg_buffer_handle h = buf_handle(0);

	kg_event_subscribe(mask, log_cb, &a);
	kg_event_subscribe(mask, log_cb, &b);
	kg_event_subscribe(mask, log_cb, &c);

	publish_mode_changed(h);
	kg_event_drain_safe();

	CHECK(count == 3);
	CHECK(log[0] == 1 && log[1] == 2 && log[2] == 3);
	CHECK(kg_event_test_max_drain_depth() == 1);
	teardown();
}

/* Records `tag`, then unsubscribes `target` -- another subscriber, or its
 * own token when `target == ` the token this very subscription was given
 * (the caller fills that in after registering, since a token does not
 * exist before its own kg_event_subscribe() call returns). */
struct unsub_ctx {
	int *log;
	size_t *count;
	int tag;
	struct kg_event_subscriber_token target;
};

static enum kg_event_cb_status unsub_and_log_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	struct unsub_ctx *ctx = ctx_;

	(void)ev;
	(void)res;
	ctx->log[(*ctx->count)++] = ctx->tag;
	kg_event_unsubscribe(ctx->target);
	return KG_EVENT_CB_CONTINUE;
}

static void test_unsubscribing_another_before_its_turn_skips_it(void)
{
	setup();
	int log[8];
	size_t count = 0;
	struct log_ctx a = { log, &count, 1 };
	struct unsub_ctx b = { log, &count, 2, KG_EVENT_SUBSCRIBER_NONE };
	struct log_ctx c = { log, &count, 3 };
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct kg_buffer_handle h = buf_handle(0);
	struct kg_event_subscriber_token tok_c;

	kg_event_subscribe(mask, log_cb, &a);
	kg_event_subscribe(mask, unsub_and_log_cb, &b);
	tok_c = kg_event_subscribe(mask, log_cb, &c);
	b.target = tok_c; /* set once C's token exists, before any dispatch */

	publish_mode_changed(h);
	kg_event_drain_safe();

	/* A: logged.  B: logged, then unsubscribed C.  C: never gets its
	 * turn for this event. */
	CHECK(count == 2);
	CHECK(log[0] == 1 && log[1] == 2);
	teardown();
}

static void test_self_unsubscribe_skips_later_events_in_the_same_drain(void)
{
	setup();
	int log[8];
	size_t count = 0;
	struct log_ctx a = { log, &count, 1 };
	struct unsub_ctx b = { log, &count, 2, KG_EVENT_SUBSCRIBER_NONE };
	struct log_ctx c = { log, &count, 3 };
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct kg_buffer_handle h = buf_handle(0);
	struct kg_event_subscriber_token tok_b;

	kg_event_subscribe(mask, log_cb, &a);
	tok_b = kg_event_subscribe(mask, unsub_and_log_cb, &b);
	kg_event_subscribe(mask, log_cb, &c);
	b.target = tok_b; /* B unsubscribes itself on its first call */

	publish_mode_changed(h);
	publish_mode_changed(h);
	kg_event_drain_safe();

	/* Event 1: A, B, C all run (B then unsubscribes itself).  Event 2,
	 * same drain, same snapshot: B is re-checked live before its call
	 * and is no longer active, so only A and C run. */
	CHECK(count == 5);
	CHECK(log[0] == 1 && log[1] == 2 && log[2] == 3);
	CHECK(log[3] == 1 && log[4] == 3);
	teardown();
}

static int recursive_drain_calls;

static enum kg_event_cb_status recursive_drain_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	(void)ev;
	(void)res;
	(void)ctx_;
	recursive_drain_calls++;
	kg_event_drain_safe(); /* must no-op: a drain is already running */
	return KG_EVENT_CB_CONTINUE;
}

static void test_drain_never_recurses(void)
{
	setup();
	recursive_drain_calls = 0;
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct kg_buffer_handle h = buf_handle(0);

	kg_event_subscribe(mask, recursive_drain_cb, NULL);
	publish_mode_changed(h);
	kg_event_drain_safe();

	CHECK(recursive_drain_calls == 1);
	CHECK(kg_event_test_max_drain_depth() == 1);
	teardown();
}

/* Queues one text change on `buf` the first time it runs, and counts
 * KG_EVENT_MODE_CHANGED deliveries -- proof that work a callback queues
 * waits for the *next* kg_event_drain_safe(), never the one it ran in. */
struct defer_ctx {
	struct kg_buffer_handle buf;
	int mode_events;
};

static enum kg_event_cb_status queue_change_once_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	struct defer_ctx *ctx = ctx_;

	(void)res;
	if (ev->kind != KG_EVENT_MODE_CHANGED) {
		return KG_EVENT_CB_CONTINUE;
	}
	ctx->mode_events++;
	if (ctx->mode_events == 1) {
		kg_event_queue_text_change(ctx->buf, 0, 0, 1,
		    (struct kg_event_buffer_extent) {
			.old_total_len = 0, .new_total_len = 1 },
		    1, CMD_ID_NONE);
	}
	return KG_EVENT_CB_CONTINUE;
}

/* Increments the int `ctx` points at and returns CONTINUE -- a subscriber
 * that just counts deliveries, for tests that care how many, not what
 * order. */
static enum kg_event_cb_status count_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	int *n = ctx_;

	(void)ev;
	(void)res;
	(*n)++;
	return KG_EVENT_CB_CONTINUE;
}

static void test_events_queued_by_a_callback_wait_for_the_next_drain(void)
{
	setup();
	struct kg_buffer_handle h = buf_handle(0);
	struct defer_ctx ctx = { h, 0 };
	int change_count = 0;
	unsigned watch_change = 1u << KG_EVENT_BUFFER_CHANGED;
	unsigned mask = (1u << KG_EVENT_MODE_CHANGED) | watch_change;

	kg_event_subscribe(mask, queue_change_once_cb, &ctx);
	kg_event_subscribe(watch_change, count_cb, &change_count);

	publish_mode_changed(h);
	kg_event_drain_safe();
	CHECK(ctx.mode_events == 1);
	/* Queued from inside the drain that just ran: newer than the
	 * boundary it captured at its own start, so not delivered yet. */
	CHECK(change_count == 0);

	kg_event_drain_safe();
	CHECK(change_count == 1);
	teardown();
}

/* Same shape as the previous test, but the callback-queued work is enough
 * to collapse into an overflow summary rather than an exact ring entry --
 * "Overflow summaries obey the same [drain] boundary" is its own line in
 * the plan, not a corollary of the exact-event case, since the overflow
 * table is a second path kg_event_drain_safe()'s bounded pop has to gate
 * too. */
static enum kg_event_cb_status fill_ring_once_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	struct defer_ctx *ctx = ctx_;
	int i;

	(void)res;
	if (ev->kind != KG_EVENT_MODE_CHANGED) {
		return KG_EVENT_CB_CONTINUE;
	}
	ctx->mode_events++;
	if (ctx->mode_events != 1) {
		return KG_EVENT_CB_CONTINUE;
	}
	/* Well past the ring's physical capacity, so at least the tail of
	 * this run collapses into `ctx->buf`'s overflow summary regardless
	 * of how much droppable headroom this drain started with. */
	for (i = 0; i < KG_EVENT_RING_MAX; i++) {
		kg_event_queue_text_change(ctx->buf, 0, 0, 1,
		    (struct kg_event_buffer_extent) { .old_total_len = 0,
			.new_total_len = (uint64_t)(i + 1) },
		    (uint64_t)(i + 1), CMD_ID_NONE);
	}
	return KG_EVENT_CB_CONTINUE;
}

static void test_overflow_created_during_a_drain_waits_for_the_next_one(void)
{
	setup();
	struct kg_buffer_handle h = buf_handle(0);
	struct defer_ctx ctx = { h, 0 };
	int change_count = 0;
	unsigned watch_change = (1u << KG_EVENT_BUFFER_CHANGED)
	    | (1u << KG_EVENT_BUFFER_BROAD_CHANGE);

	kg_event_subscribe(
	    1u << KG_EVENT_MODE_CHANGED, fill_ring_once_cb, &ctx);
	kg_event_subscribe(watch_change, count_cb, &change_count);

	publish_mode_changed(h);
	kg_event_drain_safe();
	CHECK(change_count == 0);

	kg_event_drain_safe();
	CHECK(change_count > 0);
	teardown();
}

static void test_drain_defers_while_a_prompt_is_open(void)
{
	setup();
	int log[1];
	size_t count = 0;
	struct log_ctx ctx = { log, &count, 1 };
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct kg_buffer_handle h = buf_handle(0);

	kg_event_subscribe(mask, log_cb, &ctx);
	publish_mode_changed(h);

	kg_event_prompt_enter();
	kg_event_drain_safe();
	CHECK(count == 0);

	/* Nested: a prompt started from inside another (e.g. a confirmation
	 * read while a path prompt is open) still defers until the
	 * *outermost* one leaves. */
	kg_event_prompt_enter();
	kg_event_drain_safe();
	CHECK(count == 0);
	kg_event_prompt_leave();
	kg_event_drain_safe();
	CHECK(count == 0);

	/* Leaving the outermost prompt restores eligibility; it does not
	 * itself drain -- the next explicit kg_event_drain_safe() does. */
	kg_event_prompt_leave();
	CHECK(count == 0);
	kg_event_drain_safe();
	CHECK(count == 1);
	teardown();
}

/* Records `res` at `*ctx`, whatever the event -- the whole point being
 * that a test controls what it publishes and reads back only the
 * resolution kg_event_drain_safe() computed for it. */
static enum kg_event_cb_status record_resolution_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	enum kg_event_resolution *out = ctx_;

	(void)ev;
	*out = res;
	return KG_EVENT_CB_CONTINUE;
}

static void test_resolution_is_live_then_gone_as_the_handle_dies(void)
{
	setup();
	int prev_count = buf_count;
	buf_count = 2;
	buf_attach_view(&winlist[0], 0);
	kg_event_drain_safe(); /* discard the attach event: only this test's
				* own traffic below matters */

	enum kg_event_resolution open_res = (enum kg_event_resolution) - 1;
	kg_event_subscribe(
	    1u << KG_EVENT_BUFFER_OPENED, record_resolution_cb, &open_res);

	CHECK(buf_select(1) == 1);
	kg_event_drain_safe();
	CHECK(open_res == KG_EVENT_RESOLVED_LIVE);

	/* KG_EVENT_BUFFER_KILLING's resolution is the same ordinary
	 * re-resolve, not a hard-coded answer for the kind: published by
	 * hand here (buf_kill() itself always completes -- detach, free,
	 * generation bump, KG_EVENT_BUFFER_KILLED -- before this suite ever
	 * gets to drain, so its own KILLING has always died by drain time
	 * too) for a handle nothing else has touched, it comes back live. */
	struct kg_buffer_handle h = buf_handle(1);
	enum kg_event_resolution killing_res = (enum kg_event_resolution) - 1;
	struct kg_event_reservation res;

	kg_event_subscribe(
	    1u << KG_EVENT_BUFFER_KILLING, record_resolution_cb, &killing_res);
	res = kg_event_reserve_lifecycle();
	CHECK(res.valid);
	kg_event_publish_lifecycle(&res, kg_event_make_buffer_killing(h));
	kg_event_drain_safe();
	CHECK(killing_res == KG_EVENT_RESOLVED_LIVE);

	buf_select(0);
	memset(&buflist[1], 0, sizeof(buflist[1]));
	buf_count = prev_count;
	teardown();
}

static void test_killed_event_resolves_gone(void)
{
	setup();
	int prev_count = buf_count;
	buf_count = 2;
	buf_attach_view(&winlist[0], 0);
	kg_event_drain_safe();

	enum kg_event_resolution kill_res = (enum kg_event_resolution) - 1;
	kg_event_subscribe(
	    1u << KG_EVENT_BUFFER_KILLED, record_resolution_cb, &kill_res);

	CHECK(buf_select(1) == 1);
	kg_event_drain_safe();

	buf_kill(0);
	kg_event_drain_safe();
	/* Cleanup already ran and the slot's generation already moved on by
	 * the time KG_EVENT_BUFFER_KILLED was published -- see
	 * buf_kill_commit() -- so this is expected not to resolve. */
	CHECK(kill_res == KG_EVENT_RESOLVED_GONE);

	buf_select(0);
	memset(&buflist[1], 0, sizeof(buflist[1]));
	buf_count = prev_count;
	teardown();
}

static void test_one_callback_s_error_does_not_stop_later_subscribers(void)
{
	setup();
	int log[2];
	size_t count = 0;
	struct log_ctx before = { log, &count, 1 };
	struct log_ctx after = { log, &count, 2 };
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct kg_buffer_handle h = buf_handle(0);

	kg_event_subscribe(mask, log_cb, &before);
	kg_event_subscribe(mask, error_cb, NULL);
	kg_event_subscribe(mask, log_cb, &after);

	publish_mode_changed(h);
	kg_event_drain_safe();

	CHECK(count == 2);
	CHECK(log[0] == 1 && log[1] == 2);
	CHECK(kg_event_test_error_count() == 1);
	teardown();
}

static void
test_unsubscribe_is_idempotent_and_stale_tokens_cannot_reach_a_new_subscriber(
    void)
{
	setup();
	int log_a[1];
	size_t count_a = 0;
	struct log_ctx ctx_a = { log_a, &count_a, 1 };
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;

	struct kg_event_subscriber_token tok_a
	    = kg_event_subscribe(mask, log_cb, &ctx_a);
	CHECK(kg_event_unsubscribe(tok_a)); /* removes it */
	CHECK(!kg_event_unsubscribe(tok_a)); /* idempotent: already gone */

	int log_b[1];
	size_t count_b = 0;
	struct log_ctx ctx_b = { log_b, &count_b, 2 };
	struct kg_event_subscriber_token tok_b
	    = kg_event_subscribe(mask, log_cb, &ctx_b);
	CHECK(tok_b.slot == tok_a.slot); /* the freed slot really was reused */
	CHECK(tok_b.generation != tok_a.generation);
	CHECK(!kg_event_unsubscribe(tok_a)); /* stale: same slot, old token */

	publish_mode_changed(buf_handle(0));
	kg_event_drain_safe();
	CHECK(count_b == 1); /* B is still registered and ran */
	teardown();
}

static void test_registry_exhaustion_refuses_further_registration(void)
{
	setup();
	struct kg_event_subscriber_token toks[KG_EVENT_MAX_SUBSCRIBERS];
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	int i;

	CHECK(kg_event_subscribe(mask, NULL, NULL).generation == 0);

	for (i = 0; i < KG_EVENT_MAX_SUBSCRIBERS; i++) {
		toks[i] = kg_event_subscribe(mask, noop_cb, NULL);
		CHECK(toks[i].generation != 0);
	}
	struct kg_event_subscriber_token overflow_tok
	    = kg_event_subscribe(mask, noop_cb, NULL);
	CHECK(overflow_tok.slot == 0 && overflow_tok.generation == 0);

	/* Freeing one slot makes room for exactly one more. */
	CHECK(kg_event_unsubscribe(toks[0]));
	struct kg_event_subscriber_token reused
	    = kg_event_subscribe(mask, noop_cb, NULL);
	CHECK(reused.generation != 0);
	teardown();
}

struct register_during_ctx {
	int *count_new;
	unsigned mask;
};

static enum kg_event_cb_status register_during_drain_cb(
    const struct kg_event *ev, enum kg_event_resolution res, void *ctx_)
{
	struct register_during_ctx *ctx = ctx_;

	(void)ev;
	(void)res;
	kg_event_subscribe(ctx->mask, count_cb, ctx->count_new);
	return KG_EVENT_CB_CONTINUE;
}

static void test_a_subscriber_registered_during_a_drain_waits_for_the_next_one(
    void)
{
	setup();
	int count_new = 0;
	unsigned mask = 1u << KG_EVENT_MODE_CHANGED;
	struct register_during_ctx ctx = { &count_new, mask };
	struct kg_buffer_handle h = buf_handle(0);

	kg_event_subscribe(mask, register_during_drain_cb, &ctx);

	publish_mode_changed(h);
	publish_mode_changed(h);
	kg_event_drain_safe();
	CHECK(count_new == 0); /* registered mid-drain: not this drain's */

	publish_mode_changed(h);
	kg_event_drain_safe();
	CHECK(count_new >= 1); /* eligible from the next drain on */
	teardown();
}

/* Pins the regression this slice exists to fix: undrained, the ring fills
 * from ordinary edit traffic (KG_EVENT_RING_MAX minus the reserve, 60
 * entries -- coalescing keeps genuinely distinct edits from collapsing,
 * see kg_event_queue_text_change()) and then KG_EVENT_LIFECYCLE_RESERVE
 * (4) more lifecycle events fill the rest, at which point a fifth is
 * refused -- exactly what stopped saves and opens in a long session
 * before this slice wired kg_event_drain_safe() into src/main.c's loop.
 * See also test/pty/event-drain-save-after-sustained-editing.yaml, which
 * pins the same regression end-to-end through the real producers and a
 * real save. */
static void test_drain_relieves_the_reserve_a_sustained_session_would_exhaust(
    void)
{
	setup();
	struct kg_buffer_handle h = buf_handle(0);
	struct kg_event_reservation res[KG_EVENT_LIFECYCLE_RESERVE];
	struct kg_event_reservation refused;
	struct kg_event_reservation after_drain;
	int i;

	/* 65: past the droppable share (60), so the ring is pinned at its
	 * ceiling and the tail overflow-collapses -- pressure either way. */
	for (i = 0; i < 65; i++) {
		kg_event_queue_text_change(h, 0, 0, 1,
		    (struct kg_event_buffer_extent) { .old_total_len = 0,
			.new_total_len = (uint64_t)(i + 1) },
		    (uint64_t)(i + 1), CMD_ID_NONE);
	}

	for (i = 0; i < KG_EVENT_LIFECYCLE_RESERVE; i++) {
		res[i] = kg_event_reserve_lifecycle();
		CHECK(res[i].valid);
		kg_event_publish_lifecycle(
		    &res[i], kg_event_make_before_save(h));
	}
	refused = kg_event_reserve_lifecycle();
	CHECK(!refused.valid); /* the regression: a 5th is refused */

	/* What src/main.c now calls after every command -- no subscriber
	 * needs to be registered for it to relieve the ring. */
	kg_event_drain_safe();

	after_drain = kg_event_reserve_lifecycle();
	CHECK(after_drain.valid);
	kg_event_release_reservation(&after_drain);
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
	RUN(test_producer_open_queues_resolvable_handle);
	RUN(test_producer_kill_events_killing_resolves_killed_does_not);
	RUN(test_producer_attach_detach_carry_window_and_buffer);
	RUN(test_producer_failed_save_produces_before_and_unsuccessful_after);
	RUN(test_producer_mode_changed_carries_name);
	RUN(test_producer_reservation_failure_refuses_open);
	RUN(test_producer_reservation_failure_refuses_kill);
	RUN(test_drain_dispatches_subscribers_in_registration_order);
	RUN(test_unsubscribing_another_before_its_turn_skips_it);
	RUN(test_self_unsubscribe_skips_later_events_in_the_same_drain);
	RUN(test_drain_never_recurses);
	RUN(test_events_queued_by_a_callback_wait_for_the_next_drain);
	RUN(test_overflow_created_during_a_drain_waits_for_the_next_one);
	RUN(test_drain_defers_while_a_prompt_is_open);
	RUN(test_resolution_is_live_then_gone_as_the_handle_dies);
	RUN(test_killed_event_resolves_gone);
	RUN(test_one_callback_s_error_does_not_stop_later_subscribers);
	RUN(test_unsubscribe_is_idempotent_and_stale_tokens_cannot_reach_a_new_subscriber);
	RUN(test_registry_exhaustion_refuses_further_registration);
	RUN(test_a_subscriber_registered_during_a_drain_waits_for_the_next_one);
	RUN(test_drain_relieves_the_reserve_a_sustained_session_would_exhaust);
	return tests_failed ? 1 : 0;
}
