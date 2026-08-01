/* test_decor.c — unit and reference-model tests for compact decorations */

#include "../src/bufhandle.h"
#include "../src/decor.h"
#include "../src/def.h"
#include "../src/edit.h"
#include "../src/marker.h"
#include "test.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setup(void)
{
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	reset_current_buffer();
	bcur()->active = 1;
	memset(&editor, 0, sizeof(editor));
	wcur()->active = 1;
	wcur()->h = 24;
	wcur()->w = 80;
	undo_free();
	undo_init();
}

static void teardown(void)
{
	undo_free();
	free_all_rows();
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
}

static void test_decor_create_delete_stale(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);

	struct kg_decor_handle d
	    = kg_decor_create(bcur(), 0, 5, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 10, false);
	CHECK(d.id != 0);

	struct kg_decor_info info;
	CHECK(kg_decor_resolve(d, &info) == KG_DECOR_OK);
	CHECK(info.start == 0);
	CHECK(info.end == 5);
	CHECK(info.face == KG_DECOR_FACE_MATCH);
	CHECK(info.priority == 10);
	CHECK(!info.evaporate);
	CHECK(kg_decor_resolve(d, NULL) == KG_DECOR_OK);

	CHECK(kg_decor_delete(d));
	CHECK(kg_decor_resolve(d, &info) == KG_DECOR_GONE);
	CHECK(!kg_decor_delete(d)); /* already gone */

	struct kg_decor_handle stale = { { 99, 99, 99 }, 99, 99 };
	CHECK(kg_decor_resolve(stale, NULL) == KG_DECOR_GONE);

	struct kg_decor_handle bad_face
	    = kg_decor_create(bcur(), 0, 5, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, (enum kg_decor_face)99, 0, false);
	CHECK(bad_face.id == 0);

	/* A buffer object outside buflist[] never resolves to a handle, so
	 * the two markers this stages can never be created; unlike
	 * src/marker.h, that refusal is discovered only once kg_marker_create()
	 * itself runs, after the store's own capacity was already staged, so
	 * the store may exist here -- empty, and freed below -- rather than
	 * stay NULL. */
	struct editor_buffer detached = { .active = 1, .id = 99 };
	struct kg_decor_handle refused
	    = kg_decor_create(&detached, 0, 1, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(refused.id == 0);
	CHECK(!detached.decorations || detached.decorations->count == 0);
	kg_decor_store_free(&detached);

	teardown();
}

static void test_decor_normalizes_reversed_endpoints(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcdefghij", 10);

	/* Endpoints reversed; gravity travels with the position it named. */
	struct kg_decor_handle d
	    = kg_decor_create(bcur(), 8, 2, KG_MARKER_GRAV_LEFT,
		KG_MARKER_GRAV_RIGHT, KG_DECOR_FACE_WARNING, 3, false);
	CHECK(d.id != 0);

	struct kg_decor_info info;
	CHECK(kg_decor_resolve(d, &info) == KG_DECOR_OK);
	CHECK(info.start == 2);
	CHECK(info.end == 8);

	/* Insertion at position 2 (now the start): the gravity that was
	 * named for position 2 was KG_MARKER_GRAV_RIGHT (it travelled with
	 * the swap), so the start marker advances past the new text. */
	struct kg_edit e = kg_edit_user(bcur(), 2, 2, "Z", 1);
	CHECK(kg_buffer_replace(&e, NULL) == 1);
	CHECK(kg_decor_resolve(d, &info) == KG_DECOR_OK);
	CHECK(info.start == 3);
	CHECK(info.end == 9);

	teardown();
}

/* Both endpoint gravities: the documented match-highlight default
 * (KG_MARKER_GRAV_RIGHT start, KG_MARKER_GRAV_LEFT end) excludes text
 * typed at either boundary; the opposite pair includes it. */
static void test_decor_endpoint_gravities(void)
{
	setup();
	editor_insert_row(bcur(), 0, "MATCH", 5);

	struct kg_decor_handle excluding
	    = kg_decor_create(bcur(), 0, 5, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle including
	    = kg_decor_create(bcur(), 0, 5, KG_MARKER_GRAV_LEFT,
		KG_MARKER_GRAV_RIGHT, KG_DECOR_FACE_MATCH, 0, false);

	/* Insert "X" at the shared start boundary (position 0). */
	struct kg_edit at_start = kg_edit_user(bcur(), 0, 0, "X", 1);
	CHECK(kg_buffer_replace(&at_start, NULL) == 1);

	struct kg_decor_info info;
	CHECK(kg_decor_resolve(excluding, &info) == KG_DECOR_OK);
	CHECK(info.start == 1 && info.end == 6); /* "X" excluded */
	CHECK(kg_decor_resolve(including, &info) == KG_DECOR_OK);
	CHECK(info.start == 0 && info.end == 6); /* "X" included */

	/* Buffer is now "XMATCH" (6 bytes).  Insert "Y" at the shared end
	 * boundary, position 6. */
	struct kg_edit at_end = kg_edit_user(bcur(), 6, 6, "Y", 1);
	CHECK(kg_buffer_replace(&at_end, NULL) == 1);

	CHECK(kg_decor_resolve(excluding, &info) == KG_DECOR_OK);
	CHECK(info.start == 1 && info.end == 6); /* "Y" excluded */
	CHECK(kg_decor_resolve(including, &info) == KG_DECOR_OK);
	CHECK(info.start == 0 && info.end == 7); /* "Y" included */

	teardown();
}

/* Live records stay sorted by (resolved start, end, priority, id); new
 * records use binary insertion, and overlaps/priority ties settle the
 * order by id. */
static void test_decor_overlaps_and_priority_ties(void)
{
	setup();
	editor_insert_row(bcur(), 0, "0123456789", 10);

	struct kg_decor_handle low_first
	    = kg_decor_create(bcur(), 2, 6, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 5, false);
	struct kg_decor_handle low_second
	    = kg_decor_create(bcur(), 2, 6, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 5, false);
	struct kg_decor_handle high
	    = kg_decor_create(bcur(), 2, 6, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 9, false);
	struct kg_decor_handle overlapping
	    = kg_decor_create(bcur(), 0, 4, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle nested
	    = kg_decor_create(bcur(), 3, 4, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);

	CHECK(low_first.id != 0 && low_second.id != 0 && high.id != 0
	    && overlapping.id != 0 && nested.id != 0);
	CHECK(low_first.id < low_second.id); /* same key: tie breaks by id */

	struct kg_decor_store *store = bcur()->decorations;
	CHECK(store->count == 5);

	/* Expected order: [0,4) prio0, [2,6) prio5 (id low_first), [2,6)
	 * prio5 (id low_second), [2,6) prio9, [3,4) prio0. */
	size_t s, e;
	CHECK(kg_marker_resolve(store->items[0].start, &s) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(store->items[0].end, &e) == KG_MARKER_OK);
	CHECK(s == 0 && e == 4);
	CHECK(store->items[0].id == overlapping.id);

	CHECK(kg_marker_resolve(store->items[1].start, &s) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(store->items[1].end, &e) == KG_MARKER_OK);
	CHECK(s == 2 && e == 6);
	CHECK(store->items[1].id == low_first.id);
	CHECK(store->items[2].id == low_second.id);
	CHECK(store->items[3].id == high.id);

	CHECK(kg_marker_resolve(store->items[4].start, &s) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(store->items[4].end, &e) == KG_MARKER_OK);
	CHECK(s == 3 && e == 4);
	CHECK(store->items[4].id == nested.id);

	teardown();
}

/* After an edit, kg_buffer_replace()'s call to kg_decor_compact() must
 * leave the sort invariant intact -- marker relocation is monotonic, so
 * the vector never needs a re-sort, only the assertion that it stayed
 * sorted. */
static void test_decor_stays_sorted_through_edits(void)
{
	setup();
	editor_insert_text_at_point("abcdefghijklmnopqrstuvwxyz", 26);

	for (int i = 0; i < 8; i++) {
		size_t start = (size_t)(i * 2);
		kg_decor_create(bcur(), start, start + 3, KG_MARKER_GRAV_RIGHT,
		    KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, (uint8_t)(i % 3),
		    false);
	}

	struct kg_edit e1 = kg_edit_user(bcur(), 5, 5, "XYZ", 3);
	CHECK(kg_buffer_replace(&e1, NULL) == 1);
	struct kg_edit e2 = kg_edit_user(bcur(), 10, 14, "", 0);
	CHECK(kg_buffer_replace(&e2, NULL) == 1);

	struct kg_decor_store *store = bcur()->decorations;
	size_t prev_s = 0, prev_e = 0;
	uint8_t prev_p = 0;
	uint32_t prev_id = 0;
	bool have_prev = false;

	for (size_t i = 0; i < store->count; i++) {
		size_t s, e;
		CHECK(kg_marker_resolve(store->items[i].start, &s)
		    == KG_MARKER_OK);
		CHECK(
		    kg_marker_resolve(store->items[i].end, &e) == KG_MARKER_OK);
		if (have_prev) {
			bool ok = prev_s < s || (prev_s == s && prev_e < e)
			    || (prev_s == s && prev_e == e
				&& prev_p < store->items[i].priority)
			    || (prev_s == s && prev_e == e
				&& prev_p == store->items[i].priority
				&& prev_id < store->items[i].id);
			CHECK(ok);
		}
		prev_s = s;
		prev_e = e;
		prev_p = store->items[i].priority;
		prev_id = store->items[i].id;
		have_prev = true;
	}

	teardown();
}

/* Evaporation: an evaporating decoration disappears, markers and all,
 * once an edit leaves it empty; a non-evaporating one just sits empty. */
static void test_decor_evaporation_of_empty_spans(void)
{
	setup();
	editor_insert_row(bcur(), 0, "0123456789", 10);

	struct kg_decor_handle evaporating
	    = kg_decor_create(bcur(), 3, 6, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, true);
	struct kg_decor_handle persisting
	    = kg_decor_create(bcur(), 3, 6, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);

	/* Delete [3,6): both markers collapse to position 3. */
	struct kg_edit e = kg_edit_user(bcur(), 3, 6, "", 0);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	CHECK(kg_decor_resolve(evaporating, NULL) == KG_DECOR_GONE);
	struct kg_decor_info info;
	CHECK(kg_decor_resolve(persisting, &info) == KG_DECOR_OK);
	CHECK(info.start == 3 && info.end == 3);

	CHECK(bcur()->decorations->count == 1);

	teardown();
}

/* Creation stages the store's capacity and both endpoint markers before
 * publishing; a failure at any staged step deletes what it staged and
 * leaves the decoration store exactly as it was. */
static void test_decor_allocation_failure_rollback(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	/* decor.c's own allocator makes at most two calls per creation
	 * attempt starting from an empty store: the store's first calloc
	 * (idx 0), then decor_store_reserve()'s realloc (idx 1).  Each
	 * iteration starts from a freed store, or idx 1 would find a store
	 * idx 0's own run already allocated and only exercise the realloc. */
	for (int alloc_idx = 0; alloc_idx < 2; alloc_idx++) {
		kg_decor_store_free(bcur());
		kg_decor_fail_alloc_after(alloc_idx);
		struct kg_decor_handle d
		    = kg_decor_create(bcur(), 1, 3, KG_MARKER_GRAV_RIGHT,
			KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
		CHECK(d.id == 0);
		CHECK(!bcur()->decorations || bcur()->decorations->count == 0);
	}
	kg_decor_fail_alloc_after(-1);
	kg_decor_store_free(bcur());

	struct kg_decor_handle ok
	    = kg_decor_create(bcur(), 1, 3, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(ok.id != 0);
	CHECK(bcur()->decorations->count == 1);

	teardown();
}

/* The second endpoint marker failing (exhausted IDs) rolls back the
 * first, leaving no decoration published and no marker leaked. */
static void test_decor_second_marker_failure_rolls_back_first(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	/* Prime the marker store, then arrange for exactly one more marker
	 * to succeed before exhaustion. */
	struct kg_marker_handle primer
	    = kg_marker_create(bcur(), 0, KG_MARKER_GRAV_LEFT);
	CHECK(primer.id != 0);
	bcur()->markers->next_id = UINT32_MAX - 1;

	size_t markers_before = bcur()->markers->count;
	struct kg_decor_handle d
	    = kg_decor_create(bcur(), 1, 3, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(d.id == 0);
	CHECK(!bcur()->decorations || bcur()->decorations->count == 0);
	/* The one marker that did get created (then rolled back) leaves the
	 * store's slot count where marker deletion always does: inactive,
	 * not shrunk. */
	CHECK(bcur()->markers->count == markers_before + 1);

	teardown();
}

static void test_decor_id_exhaustion(void)
{
	setup();
	editor_insert_row(bcur(), 0, "test", 4);

	struct kg_decor_handle d1
	    = kg_decor_create(bcur(), 0, 1, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(d1.id != 0);
	CHECK(bcur()->decorations != NULL);

	bcur()->decorations->next_id = UINT32_MAX;
	struct kg_decor_handle refused
	    = kg_decor_create(bcur(), 1, 2, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(refused.id == 0);
	CHECK(bcur()->decorations->count == 1);
	CHECK(kg_decor_resolve(d1, NULL) == KG_DECOR_OK);

	teardown();
}

/* Broad row adoption drops every decoration along with its endpoint
 * markers, the same way it detaches every ordinary marker. */
static void test_decor_broad_adoption_drops_decorations(void)
{
	setup();
	editor_insert_row(bcur(), 0, "old text", 8);

	struct kg_decor_handle d
	    = kg_decor_create(bcur(), 1, 4, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(d.id != 0);

	erow *rows = NULL;
	int numrows = 0, capacity = 0;
	struct editor_buffer staged = { .active = 1 };
	editor_insert_row(&staged, 0, "new", 3);
	rows = staged.row;
	numrows = staged.numrows;
	capacity = staged.row_capacity;
	kg_buffer_adopt_rows(bcur(), &rows, &numrows, &capacity);

	CHECK(kg_decor_resolve(d, NULL) == KG_DECOR_GONE);
	CHECK(bcur()->decorations->count == 0);
	CHECK(bcur()->numrows == 1);
	CHECK(memcmp(bcur()->row[0].chars, "new", 3) == 0);

	teardown();
}

/* Buffer kill invalidates the buffer handle itself, which is what
 * actually protects a decoration handle across slot reuse: a fresh
 * decoration in the reused slot gets a fresh id, but even an id
 * collision would not resolve, because the outer buffer generation no
 * longer matches. */
static void test_decor_buffer_kill_and_slot_reuse(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	struct kg_decor_handle before_kill
	    = kg_decor_create(bcur(), 0, 3, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(before_kill.id != 0);

	/* Simulate what buf_kill()/bufmgr.c's cleanup do to this slot: free
	 * both stores, then bump the slot's generation so no handle taken
	 * before this point can ever resolve into whatever reuses it. */
	kg_decor_store_free(bcur());
	kg_marker_store_free(bcur());
	bcur()->generation++;

	CHECK(kg_decor_resolve(before_kill, NULL) == KG_DECOR_GONE);

	/* Reuse: a fresh decoration in the same slot, same low id sequence,
	 * is unrelated to the old handle. */
	editor_insert_row(bcur(), 0, "reused", 6);
	struct kg_decor_handle after_reuse
	    = kg_decor_create(bcur(), 0, 3, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(after_reuse.id != 0);
	CHECK(kg_decor_resolve(after_reuse, NULL) == KG_DECOR_OK);
	CHECK(kg_decor_resolve(before_kill, NULL) == KG_DECOR_GONE);

	teardown();
}

/* Collect every span kg_decor_query_next() yields for [start,end) into
 * `out`, in the order it yields them, and return the count. */
static size_t collect_query(struct editor_buffer *b, size_t start, size_t end,
    struct kg_decor_query_span *out, size_t max)
{
	struct kg_decor_query q;
	size_t n = 0;

	kg_decor_query_begin(&q, b, start, end);
	while (n < max && kg_decor_query_next(&q, &out[n])) {
		n++;
	}
	return n;
}

/* The visible-range query returns only intersecting spans, in the same
 * (resolved start, end, priority, ID) order the store itself keeps --
 * it is a filtered walk of the sorted vector, not a re-sort. */
static void test_decor_query_returns_intersecting_spans_in_order(void)
{
	setup();
	editor_insert_row(bcur(), 0, "0123456789012345678901234567890", 32);

	struct kg_decor_handle before
	    = kg_decor_create(bcur(), 0, 3, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle low
	    = kg_decor_create(bcur(), 10, 15, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 1, false);
	struct kg_decor_handle high
	    = kg_decor_create(bcur(), 10, 15, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 9, false);
	struct kg_decor_handle spanning
	    = kg_decor_create(bcur(), 8, 25, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle after
	    = kg_decor_create(bcur(), 30, 32, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	CHECK(before.id && low.id && high.id && spanning.id && after.id);

	struct kg_decor_query_span spans[8];
	size_t n = collect_query(bcur(), 10, 20, spans, 8);

	/* `before` ends before 10; `after` starts at 30 (>= range end);
	 * neither intersects.  The three that do come back sorted:
	 * `spanning` (start 8) first, then the two tied at (start 10, end
	 * 15), low priority before high, matching store order. */
	CHECK(n == 3);
	CHECK(spans[0].id == spanning.id);
	CHECK(spans[0].start == 8 && spans[0].end == 25);
	CHECK(spans[1].id == low.id);
	CHECK(spans[1].priority == 1);
	CHECK(spans[2].id == high.id);
	CHECK(spans[2].priority == 9);
	CHECK(spans[2].face == KG_DECOR_FACE_WARNING);

	teardown();
}

/* The intersection test is half-open on both the query range and the
 * decoration's own span: a decoration ending exactly at range_start, or
 * starting exactly at range_end, does not intersect; one that properly
 * contains an interior point does, including a zero-width (point)
 * decoration strictly inside the range. */
static void test_decor_query_half_open_boundaries(void)
{
	setup();
	editor_insert_row(bcur(), 0, "0123456789", 10);

	struct kg_decor_handle ends_at_start
	    = kg_decor_create(bcur(), 2, 5, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle starts_at_end
	    = kg_decor_create(bcur(), 8, 9, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle contains_range
	    = kg_decor_create(bcur(), 0, 10, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle point_inside
	    = kg_decor_create(bcur(), 6, 6, KG_MARKER_GRAV_LEFT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 0, false);
	struct kg_decor_handle point_at_edge
	    = kg_decor_create(bcur(), 8, 8, KG_MARKER_GRAV_LEFT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 0, false);
	CHECK(ends_at_start.id && starts_at_end.id && contains_range.id
	    && point_inside.id && point_at_edge.id);

	/* Query range [5,8): only contains_range and point_inside (6) are
	 * strictly inside it; ends_at_start's end (5) and starts_at_end's
	 * start (8) both sit on the excluded boundary, and so does
	 * point_at_edge (8). */
	struct kg_decor_query_span spans[8];
	size_t n = collect_query(bcur(), 5, 8, spans, 8);

	CHECK(n == 2);
	CHECK(spans[0].id == contains_range.id);
	CHECK(spans[1].id == point_inside.id);

	teardown();
}

/* A decoration whose endpoint marker has gone stale since the query
 * began is skipped, never reported with a guessed range -- the query's
 * own version of what kg_decor_resolve() already guarantees for a single
 * handle. */
static void test_decor_query_skips_stale_endpoint(void)
{
	setup();
	editor_insert_row(bcur(), 0, "0123456789", 10);

	struct kg_decor_handle live
	    = kg_decor_create(bcur(), 2, 5, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_MATCH, 0, false);
	struct kg_decor_handle going_stale
	    = kg_decor_create(bcur(), 3, 6, KG_MARKER_GRAV_RIGHT,
		KG_MARKER_GRAV_LEFT, KG_DECOR_FACE_WARNING, 0, false);
	CHECK(live.id != 0 && going_stale.id != 0);

	/* Delete the second decoration's start marker directly, underneath
	 * it -- the record stays in the store (only kg_decor_delete() would
	 * remove that), but it can no longer resolve. */
	struct kg_decor_store *store = bcur()->decorations;
	CHECK(store->count == 2);
	for (size_t i = 0; i < store->count; i++) {
		if (store->items[i].id == going_stale.id) {
			kg_marker_delete(store->items[i].start);
		}
	}
	CHECK(kg_decor_resolve(going_stale, NULL) == KG_DECOR_GONE);

	struct kg_decor_query_span spans[4];
	size_t n = collect_query(bcur(), 0, 10, spans, 4);

	CHECK(n == 1);
	CHECK(spans[0].id == live.id);

	teardown();
}

/* A NULL buffer, and a live buffer with no decoration store at all, both
 * walk as "done" on the first poll -- never a crash, never a guessed
 * span. */
static void test_decor_query_empty_or_no_store(void)
{
	setup();

	struct kg_decor_query q;
	struct kg_decor_query_span span;

	kg_decor_query_begin(&q, NULL, 0, 100);
	CHECK(!kg_decor_query_next(&q, &span));

	CHECK(bcur()->decorations == NULL);
	kg_decor_query_begin(&q, bcur(), 0, 100);
	CHECK(!kg_decor_query_next(&q, &span));

	teardown();
}

/* Reference-model test: generate arbitrary decorations over a buffer that
 * undergoes a random edit sequence, and check every live decoration's
 * resolved span against a flat-string model driven by the same gravity
 * rules test_marker.c's model already proves for lone markers. */
struct ref_endpoint {
	size_t pos;
	enum kg_marker_gravity gravity;
};

struct ref_decor {
	struct ref_endpoint start;
	struct ref_endpoint end;
	bool evaporate;
	bool alive;
	struct kg_decor_handle handle;
};

static uint32_t rng_state = 0xabcdef;
static uint32_t rand_u32(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void ref_relocate(
    struct ref_endpoint *ep, size_t begin, size_t end, size_t ins_len)
{
	size_t p = ep->pos;
	size_t old_len = end - begin;

	if (old_len == 0) {
		if (p > begin) {
			p += ins_len;
		} else if (p == begin && ep->gravity == KG_MARKER_GRAV_RIGHT) {
			p = begin + ins_len;
		}
	} else {
		if (p >= end) {
			p = p - old_len + ins_len;
		} else if (p >= begin) {
			p = (ep->gravity == KG_MARKER_GRAV_LEFT)
			    ? begin
			    : begin + ins_len;
		}
	}
	ep->pos = p;
}

static void test_decor_randomized_reference_model(void)
{
	setup();
	char ref_str[2048] = "The quick brown fox jumps over the lazy dog.\n";
	size_t ref_len = strlen(ref_str);
	editor_insert_text_at_point(ref_str, (int)ref_len);

#define NUM_REF_DECORS 16
	struct ref_decor refs[NUM_REF_DECORS];
	for (size_t i = 0; i < NUM_REF_DECORS; i++) {
		size_t a = (size_t)(rand_u32() % (ref_len + 1));
		size_t b = (size_t)(rand_u32() % (ref_len + 1));

		refs[i].start.pos = a < b ? a : b;
		refs[i].end.pos = a < b ? b : a;
		refs[i].start.gravity = KG_MARKER_GRAV_RIGHT;
		refs[i].end.gravity = KG_MARKER_GRAV_LEFT;
		refs[i].evaporate = (rand_u32() % 2) == 0;
		refs[i].alive = true;
		refs[i].handle = kg_decor_create(bcur(), refs[i].start.pos,
		    refs[i].end.pos, refs[i].start.gravity, refs[i].end.gravity,
		    KG_DECOR_FACE_MATCH, (uint8_t)(i % 4), refs[i].evaporate);
		CHECK(refs[i].handle.id != 0);
	}

	for (int step = 0; step < 40; step++) {
		size_t begin = 0, end = 0;
		if (ref_len > 0) {
			begin = (size_t)(rand_u32() % (ref_len + 1));
			end = begin
			    + (size_t)(rand_u32() % (ref_len - begin + 1));
		}
		size_t ins_len = (size_t)(rand_u32() % 12);
		char ins_buf[12];
		for (size_t k = 0; k < ins_len; k++) {
			ins_buf[k] = (char)('a' + (rand_u32() % 26));
		}
		size_t old_len = end - begin;
		int64_t delta = (int64_t)ins_len - (int64_t)old_len;

		for (size_t i = 0; i < NUM_REF_DECORS; i++) {
			if (!refs[i].alive) {
				continue;
			}
			ref_relocate(&refs[i].start, begin, end, ins_len);
			ref_relocate(&refs[i].end, begin, end, ins_len);
			if (refs[i].evaporate
			    && refs[i].start.pos == refs[i].end.pos) {
				refs[i].alive = false;
			}
		}

		struct kg_edit e = kg_edit_user(
		    bcur(), begin, end, ins_len ? ins_buf : "", ins_len);
		CHECK(kg_buffer_replace(&e, NULL) == 1);

		memmove(ref_str + begin + ins_len, ref_str + end,
		    ref_len - end + 1);
		if (ins_len > 0) {
			memcpy(ref_str + begin, ins_buf, ins_len);
		}
		ref_len = (size_t)((int64_t)ref_len + delta);

		for (size_t i = 0; i < NUM_REF_DECORS; i++) {
			enum kg_decor_result r
			    = kg_decor_resolve(refs[i].handle, NULL);
			if (refs[i].alive) {
				struct kg_decor_info info;

				CHECKF(r == KG_DECOR_OK,
				    "step=%d i=%zu expected alive", step, i);
				CHECK(kg_decor_resolve(refs[i].handle, &info)
				    == KG_DECOR_OK);
				CHECKF(info.start == refs[i].start.pos
					&& info.end == refs[i].end.pos,
				    "step=%d i=%zu got [%zu,%zu) want "
				    "[%zu,%zu)",
				    step, i, info.start, info.end,
				    refs[i].start.pos, refs[i].end.pos);
			} else {
				CHECKF(r == KG_DECOR_GONE,
				    "step=%d i=%zu expected evaporated", step,
				    i);
			}
		}
	}

	teardown();
}

int main(void)
{
	RUN(test_decor_create_delete_stale);
	RUN(test_decor_normalizes_reversed_endpoints);
	RUN(test_decor_endpoint_gravities);
	RUN(test_decor_overlaps_and_priority_ties);
	RUN(test_decor_stays_sorted_through_edits);
	RUN(test_decor_evaporation_of_empty_spans);
	RUN(test_decor_allocation_failure_rollback);
	RUN(test_decor_second_marker_failure_rolls_back_first);
	RUN(test_decor_id_exhaustion);
	RUN(test_decor_broad_adoption_drops_decorations);
	RUN(test_decor_buffer_kill_and_slot_reuse);
	RUN(test_decor_query_returns_intersecting_spans_in_order);
	RUN(test_decor_query_half_open_boundaries);
	RUN(test_decor_query_skips_stale_endpoint);
	RUN(test_decor_query_empty_or_no_store);
	RUN(test_decor_randomized_reference_model);
	return test_summary();
}
