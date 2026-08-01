/* test_marker.c — unit and reference-model tests for marker publication */

#include "../src/bufhandle.h"
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
	kg_marker_store_free(bcur());
}

static void test_marker_creation_and_resolve(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello world", 11);

	struct kg_marker_handle m1
	    = kg_marker_create(bcur(), 0, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m2
	    = kg_marker_create(bcur(), 5, KG_MARKER_GRAV_RIGHT);
	struct kg_marker_handle m3
	    = kg_marker_create(bcur(), 11, KG_MARKER_GRAV_LEFT);

	size_t pos;
	CHECK(kg_marker_resolve(m1, &pos) == KG_MARKER_OK);
	CHECK(pos == 0);
	CHECK(kg_marker_resolve(m2, &pos) == KG_MARKER_OK);
	CHECK(pos == 5);
	CHECK(kg_marker_resolve(m3, &pos) == KG_MARKER_OK);
	CHECK(pos == 11);

	int r, c;
	CHECK(kg_marker_get_row_col(m2, &r, &c) == true);
	CHECK(r == 0);
	CHECK(c == 5);

	struct kg_marker_handle invalid = { { 99, 99, 99 }, 99, 99 };
	CHECK(kg_marker_resolve(invalid, &pos) == KG_MARKER_GONE);
	CHECK(kg_marker_set_gravity(m1, (enum kg_marker_gravity)99)
	    == KG_MARKER_TYPE_ERROR);
	struct kg_marker_handle bad_gravity
	    = kg_marker_create(bcur(), 0, (enum kg_marker_gravity)99);
	CHECK(bad_gravity.id == 0);

	teardown();
}

static void test_active_mark_relocates_through_edit_and_undo(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcdef", 6);
	CHECK(kg_mark_set_position(bcur(), 4));

	struct kg_edit insert = kg_edit_user(bcur(), 1, 1, "XX", 2);
	CHECK(kg_buffer_replace(&insert, NULL) == 1);
	size_t pos;
	CHECK(kg_mark_get_position(bcur(), &pos));
	CHECK(pos == 6);

	editor_undo();
	CHECK(kg_mark_get_position(bcur(), &pos));
	CHECK(pos == 4);

	struct kg_edit erase = kg_edit_user(bcur(), 2, 6, "", 0);
	CHECK(kg_buffer_replace(&erase, NULL) == 1);
	CHECK(kg_mark_get_position(bcur(), &pos));
	CHECK(pos == 2);

	teardown();
}

static void test_marker_delete_and_slot_reuse(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	struct kg_marker_handle old
	    = kg_marker_create(bcur(), 1, KG_MARKER_GRAV_LEFT);
	CHECK(kg_marker_delete(old));

	struct kg_marker_handle replacement
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_RIGHT);
	size_t pos;
	CHECK(replacement.id != 0);
	CHECK(kg_marker_resolve(old, &pos) == KG_MARKER_GONE);
	CHECK(kg_marker_resolve(replacement, &pos) == KG_MARKER_OK);
	CHECK(pos == 2);

	teardown();
}

static void test_marker_mutation_and_detach(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abc", 3);
	struct kg_marker_handle first
	    = kg_marker_create(bcur(), 1, KG_MARKER_GRAV_LEFT);
	CHECK(kg_marker_set_position(first, 99) == KG_MARKER_OK);
	CHECK(
	    kg_marker_set_gravity(first, KG_MARKER_GRAV_RIGHT) == KG_MARKER_OK);

	size_t pos;
	enum kg_marker_gravity gravity;
	CHECK(kg_marker_resolve(first, &pos) == KG_MARKER_OK);
	CHECK(pos == 3);
	CHECK(kg_marker_get_gravity(first, &gravity) == KG_MARKER_OK);
	CHECK(gravity == KG_MARKER_GRAV_RIGHT);
	CHECK(kg_marker_get_gravity(first, NULL) == KG_MARKER_OK);
	CHECK(kg_marker_get_row_col(first, NULL, NULL));

	struct kg_marker_handle second
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_LEFT);
	kg_marker_detach_all(bcur());
	CHECK(kg_marker_resolve(first, NULL) == KG_MARKER_GONE);
	CHECK(kg_marker_resolve(second, NULL) == KG_MARKER_GONE);
	CHECK(!kg_marker_delete(first));

	struct editor_buffer detached = { .active = 1, .id = 99 };
	struct kg_marker_handle refused
	    = kg_marker_create(&detached, 0, KG_MARKER_GRAV_LEFT);
	CHECK(refused.id == 0);
	CHECK(detached.markers == NULL);

	teardown();
}

static void test_broad_adoption_detaches_marks(void)
{
	setup();
	editor_insert_row(bcur(), 0, "old", 3);
	CHECK(kg_mark_set_row_col(bcur(), 0, 9));
	bcur()->mark_ring[0] = (struct kg_buffer_mark) {
		.marker = kg_marker_create(bcur(), 1, KG_MARKER_GRAV_LEFT),
		.virtual_chars_col = -1,
	};
	bcur()->mark_ring_len = 1;
	struct kg_marker_handle ordinary
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_RIGHT);

	erow *rows = NULL;
	int numrows = 0, capacity = 0;
	struct editor_buffer staged = { .active = 1 };
	editor_insert_row(&staged, 0, "new", 3);
	rows = staged.row;
	numrows = staged.numrows;
	capacity = staged.row_capacity;
	kg_buffer_adopt_rows(bcur(), &rows, &numrows, &capacity);

	CHECK(rows == NULL);
	CHECK(numrows == 0);
	CHECK(capacity == 0);
	CHECK(!kg_mark_is_set(bcur()));
	CHECK(bcur()->mark_ring_len == 0);
	CHECK(kg_marker_resolve(ordinary, NULL) == KG_MARKER_GONE);
	CHECK(bcur()->numrows == 1);
	CHECK(memcmp(bcur()->row[0].chars, "new", 3) == 0);

	teardown();
}

static void test_marker_insertion_at_marker(void)
{
	setup();
	editor_insert_row(bcur(), 0, "helloworld", 10);

	struct kg_marker_handle m_left
	    = kg_marker_create(bcur(), 5, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m_right
	    = kg_marker_create(bcur(), 5, KG_MARKER_GRAV_RIGHT);

	/* Insert " " at pos 5 */
	struct kg_edit e = kg_edit_user(bcur(), 5, 5, " ", 1);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	size_t pos_l, pos_r;
	CHECK(kg_marker_resolve(m_left, &pos_l) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m_right, &pos_r) == KG_MARKER_OK);
	CHECK(pos_l == 5);
	CHECK(pos_r == 6);

	teardown();
}

static void test_marker_deletion_endpoints(void)
{
	setup();
	editor_insert_row(bcur(), 0, "abcdefghij", 10);

	struct kg_marker_handle m1
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m2
	    = kg_marker_create(bcur(), 4, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m3
	    = kg_marker_create(bcur(), 6, KG_MARKER_GRAV_RIGHT);
	struct kg_marker_handle m4
	    = kg_marker_create(bcur(), 8, KG_MARKER_GRAV_RIGHT);

	/* Delete range [3, 7) ("defg") */
	struct kg_edit e = kg_edit_user(bcur(), 3, 7, "", 0);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	size_t p1, p2, p3, p4;
	CHECK(kg_marker_resolve(m1, &p1) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m2, &p2) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m3, &p3) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m4, &p4) == KG_MARKER_OK);

	CHECK(p1 == 2);
	CHECK(p2 == 3);
	CHECK(p3 == 3);
	CHECK(p4 == 4);

	teardown();
}

static void test_marker_multiline_replacements(void)
{
	setup();
	editor_insert_row(bcur(), 0, "line1", 5);
	editor_insert_row(bcur(), 1, "line2", 5);
	editor_insert_row(bcur(), 2, "line3", 5);

	/* Positions: line1 is 0..5, \n is 5. line2 is 6..11, \n is 11. line3
	 * is 12..17. Total bytes: 17 */
	struct kg_marker_handle m_before
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m_mid
	    = kg_marker_create(bcur(), 8, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m_after
	    = kg_marker_create(bcur(), 14, KG_MARKER_GRAV_RIGHT);

	/* Replace "line2\n" (range 6..12, len 6) with "rowA\nrowB\n" (len 10)
	 */
	struct kg_edit e = kg_edit_user(bcur(), 6, 12, "rowA\nrowB\n", 10);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	size_t p_b, p_m, p_a;
	CHECK(kg_marker_resolve(m_before, &p_b) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m_mid, &p_m) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m_after, &p_a) == KG_MARKER_OK);

	CHECK(p_b == 2);
	CHECK(p_m == 6);
	CHECK(p_a == 18); /* 14 + (10 - 6) = 18 */

	teardown();
}

static void test_marker_utf8_and_malformed_bytes(void)
{
	setup();
	/* "a" (1), "\xC3\xA9" (2), "\x80\xFF" (2), "b" (1) -> total 6 bytes */
	char raw[] = "a\xC3\xA9"
		     "\x80"
		     "\xFF"
		     "b";
	editor_insert_row(bcur(), 0, raw, 6);

	struct kg_marker_handle m_utf8
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_RIGHT);
	struct kg_marker_handle m_malformed
	    = kg_marker_create(bcur(), 4, KG_MARKER_GRAV_LEFT);

	/* Replace '\x80' at pos 3 with 'X' */
	struct kg_edit e = kg_edit_user(bcur(), 3, 4, "X", 1);
	CHECK(kg_buffer_replace(&e, NULL) == 1);

	size_t p_u, p_m;
	CHECK(kg_marker_resolve(m_utf8, &p_u) == KG_MARKER_OK);
	CHECK(kg_marker_resolve(m_malformed, &p_m) == KG_MARKER_OK);

	CHECK(p_u == 2);
	CHECK(p_m == 4);

	teardown();
}

static void test_marker_store_free_detaches_handles(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);
	struct kg_marker_handle m
	    = kg_marker_create(bcur(), 2, KG_MARKER_GRAV_LEFT);

	size_t pos;
	CHECK(kg_marker_resolve(m, &pos) == KG_MARKER_OK);

	kg_marker_store_free(bcur());
	CHECK(kg_marker_resolve(m, &pos) == KG_MARKER_GONE);

	teardown();
}

static void test_marker_id_exhaustion(void)
{
	setup();
	editor_insert_row(bcur(), 0, "test", 4);

	struct kg_marker_handle m1
	    = kg_marker_create(bcur(), 0, KG_MARKER_GRAV_LEFT);
	CHECK(bcur()->markers != NULL);

	bcur()->markers->next_id = UINT32_MAX;
	struct kg_marker_handle m_refused
	    = kg_marker_create(bcur(), 1, KG_MARKER_GRAV_LEFT);
	CHECK(m_refused.id == 0);

	size_t pos;
	CHECK(kg_marker_resolve(m_refused, &pos) == KG_MARKER_GONE);
	CHECK(kg_marker_resolve(m1, &pos) == KG_MARKER_OK);

	kg_marker_store_free(bcur());
	teardown();
}

static void test_marker_store_growth_is_linear(void)
{
	setup();

	for (size_t i = 0; i < 1024; i++) {
		struct kg_marker_handle marker
		    = kg_marker_create(bcur(), 0, KG_MARKER_GRAV_LEFT);

		CHECK(marker.id != 0);
	}
	CHECK(bcur()->markers->count == 1024);
	CHECK(bcur()->markers->capacity == 1024);

	teardown();
}

static void test_marker_edit_allocation_failure_bit_identical(void)
{
	setup();
	editor_insert_row(bcur(), 0, "hello", 5);

	struct kg_marker_handle m1
	    = kg_marker_create(bcur(), 1, KG_MARKER_GRAV_LEFT);
	struct kg_marker_handle m2
	    = kg_marker_create(bcur(), 3, KG_MARKER_GRAV_RIGHT);

	/* Attempt edit under allocation failure seams */
	for (int alloc_idx = 0; alloc_idx < 4; alloc_idx++) {
		kg_edit_fail_alloc_after(alloc_idx);
		struct kg_edit e = kg_edit_user(bcur(), 1, 4, "WORLD", 5);
		CHECK(kg_buffer_replace(&e, NULL) == 0);

		size_t p1, p2;
		CHECK(kg_marker_resolve(m1, &p1) == KG_MARKER_OK);
		CHECK(kg_marker_resolve(m2, &p2) == KG_MARKER_OK);
		CHECK(p1 == 1);
		CHECK(p2 == 3);
	}
	kg_edit_fail_alloc_after(-1);

	teardown();
}

/* Reference model test comparing markers against flat string model */
struct ref_marker {
	size_t pos;
	enum kg_marker_gravity gravity;
	bool active;
	struct kg_marker_handle handle;
};

static uint32_t rng_state = 0x12345;
static uint32_t rand_u32(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void test_marker_randomized_reference_model(void)
{
	setup();
	char ref_str[4096] = "Initial reference string for marker testing.\n";
	size_t ref_len = strlen(ref_str);
	editor_insert_text_at_point(ref_str, (int)ref_len);

#define NUM_REF_MARKERS 32
	struct ref_marker ref_markers[NUM_REF_MARKERS];
	for (size_t i = 0; i < NUM_REF_MARKERS; i++) {
		ref_markers[i].pos = (size_t)(rand_u32() % (ref_len + 1));
		ref_markers[i].gravity = (rand_u32() % 2 == 0)
		    ? KG_MARKER_GRAV_LEFT
		    : KG_MARKER_GRAV_RIGHT;
		ref_markers[i].active = true;
		ref_markers[i].handle = kg_marker_create(
		    bcur(), ref_markers[i].pos, ref_markers[i].gravity);
	}

	for (int step = 0; step < 50; step++) {
		size_t begin = 0, end = 0;
		if (ref_len > 0) {
			begin = (size_t)(rand_u32() % (ref_len + 1));
			end = begin
			    + (size_t)(rand_u32() % (ref_len - begin + 1));
		}
		size_t ins_len = (size_t)(rand_u32() % 16);
		char ins_buf[16];
		for (size_t k = 0; k < ins_len; k++) {
			ins_buf[k] = (char)('a' + (rand_u32() % 26));
		}

		size_t old_len = end - begin;
		int64_t delta = (int64_t)ins_len - (int64_t)old_len;

		/* Update reference markers */
		for (size_t i = 0; i < NUM_REF_MARKERS; i++) {
			if (!ref_markers[i].active) {
				continue;
			}
			size_t p = ref_markers[i].pos;
			if (old_len == 0) {
				if (p > begin) {
					p += ins_len;
				} else if (p == begin) {
					if (ref_markers[i].gravity
					    == KG_MARKER_GRAV_RIGHT) {
						p = begin + ins_len;
					}
				}
			} else {
				if (p >= end) {
					p = (size_t)((int64_t)p + delta);
				} else if (p >= begin) {
					if (ref_markers[i].gravity
					    == KG_MARKER_GRAV_LEFT) {
						p = begin;
					} else {
						p = begin + ins_len;
					}
				}
			}
			ref_markers[i].pos = p;
		}

		/* Perform edit on kg buffer */
		struct kg_edit e = kg_edit_user(
		    bcur(), begin, end, ins_len ? ins_buf : "", ins_len);
		int ok = kg_buffer_replace(&e, NULL);
		CHECKF(ok == 1,
		    "step=%d begin=%zu end=%zu ins_len=%zu b_len=%zu "
		    "ref_len=%zu",
		    step, begin, end, ins_len, buffer_byte_length(bcur()),
		    ref_len);

		/* Update reference string */
		memmove(ref_str + begin + ins_len, ref_str + end,
		    ref_len - end + 1);
		if (ins_len > 0) {
			memcpy(ref_str + begin, ins_buf, ins_len);
		}
		ref_len = (size_t)((int64_t)ref_len + delta);

		/* Validate all live markers */
		for (size_t i = 0; i < NUM_REF_MARKERS; i++) {
			if (!ref_markers[i].active) {
				continue;
			}
			size_t kg_pos;
			CHECK(kg_marker_resolve(ref_markers[i].handle, &kg_pos)
			    == KG_MARKER_OK);
			CHECK(kg_pos == ref_markers[i].pos);
		}
	}

	teardown();
}

int main(void)
{
	RUN(test_marker_creation_and_resolve);
	RUN(test_active_mark_relocates_through_edit_and_undo);
	RUN(test_marker_delete_and_slot_reuse);
	RUN(test_marker_mutation_and_detach);
	RUN(test_broad_adoption_detaches_marks);
	RUN(test_marker_insertion_at_marker);
	RUN(test_marker_deletion_endpoints);
	RUN(test_marker_multiline_replacements);
	RUN(test_marker_utf8_and_malformed_bytes);
	RUN(test_marker_store_free_detaches_handles);
	RUN(test_marker_id_exhaustion);
	RUN(test_marker_store_growth_is_linear);
	RUN(test_marker_edit_allocation_failure_bit_identical);
	RUN(test_marker_randomized_reference_model);
	return test_summary();
}
