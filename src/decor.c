/* decor.c — compact, marker-anchored buffer decorations */

#include "decor.h"
#include "def.h"
#include "perf.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define KG_DECOR_FLAG_START_RIGHT (1u << 0)
#define KG_DECOR_FLAG_END_RIGHT (1u << 1)
#define KG_DECOR_FLAG_EVAPORATE (1u << 2)

/* How many more allocations this module's own store growth may make
 * before one of them is failed on purpose.  Negative is how the editor
 * always runs: no seam.  See kg_decor_fail_alloc_after() in decor.h;
 * mirrors edit_alloc_budget in buffer.c, one seam per allocator. */
static int decor_alloc_budget = -1;

void kg_decor_fail_alloc_after(int n) { decor_alloc_budget = n; }

static bool decor_alloc_ok(void)
{
	return decor_alloc_budget < 0 || decor_alloc_budget-- != 0;
}

static bool decor_face_valid(enum kg_decor_face face)
{
	/* The faces are a contiguous range with a named first and last
	 * (decor.h), so this is two comparisons rather than one clause per
	 * face -- which is also what keeps this function's cost where it was
	 * when there were two of them. */
	return face >= KG_DECOR_FACE_MATCH
	    && face <= KG_DECOR_FACE_PAREN_MISMATCH;
}

static bool decor_store_reserve(struct kg_decor_store *store)
{
	size_t old_cap, new_cap;
	struct kg_decor *items;

	if (store->count < store->capacity) {
		return true;
	}
	old_cap = store->capacity;
	if (old_cap > SIZE_MAX / 2) {
		return false;
	}
	new_cap = old_cap ? old_cap * 2 : 8;
	if (new_cap > SIZE_MAX / sizeof(*store->items)) {
		return false;
	}
	items = decor_alloc_ok()
	    ? realloc(store->items, new_cap * sizeof(*items))
	    : NULL;
	if (!items) {
		return false;
	}
	store->items = items;
	store->capacity = new_cap;
	return true;
}

static struct kg_decor *decor_store_find(struct kg_decor_store *store,
    uint32_t id, uint32_t generation, size_t *out_idx)
{
	for (size_t i = 0; i < store->count; i++) {
		if (store->items[i].id == id
		    && store->items[i].generation == generation) {
			if (out_idx) {
				*out_idx = i;
			}
			return &store->items[i];
		}
	}
	return NULL;
}

/* The live sort key: resolved start, then end, then priority, then ID --
 * the order kg_decor_compact() relies on marker relocation to preserve. */
static bool decor_key_less(size_t s1, size_t e1, uint8_t p1, uint32_t id1,
    size_t s2, size_t e2, uint8_t p2, uint32_t id2)
{
	if (s1 != s2) {
		return s1 < s2;
	}
	if (e1 != e2) {
		return e1 < e2;
	}
	if (p1 != p2) {
		return p1 < p2;
	}
	return id1 < id2;
}

/* First index whose resolved key is not less than (start, end, priority,
 * id), i.e. where a new record with that key belongs. */
static size_t decor_insert_index(struct kg_decor_store *store, size_t start,
    size_t end, uint8_t priority, uint32_t id)
{
	size_t lo = 0, hi = store->count;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		size_t s = 0, e = 0;

		kg_marker_resolve(store->items[mid].start, &s);
		kg_marker_resolve(store->items[mid].end, &e);
		if (decor_key_less(s, e, store->items[mid].priority,
			store->items[mid].id, start, end, priority, id)) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

static uint8_t decor_flags(enum kg_marker_gravity start_gravity,
    enum kg_marker_gravity end_gravity, bool evaporate)
{
	uint8_t flags = 0;

	if (start_gravity == KG_MARKER_GRAV_RIGHT) {
		flags |= KG_DECOR_FLAG_START_RIGHT;
	}
	if (end_gravity == KG_MARKER_GRAV_RIGHT) {
		flags |= KG_DECOR_FLAG_END_RIGHT;
	}
	if (evaporate) {
		flags |= KG_DECOR_FLAG_EVAPORATE;
	}
	return flags;
}

struct kg_decor_handle kg_decor_create(struct editor_buffer *b,
    size_t start_pos, size_t end_pos, enum kg_marker_gravity start_gravity,
    enum kg_marker_gravity end_gravity, enum kg_decor_face face,
    uint8_t priority, bool evaporate)
{
	struct kg_decor_handle null_handle = { { -1, 0, 0 }, 0, 0 };
	struct kg_decor_store *store;
	struct kg_marker_handle start, end;
	struct kg_decor rec;
	size_t idx;

	if (!b || !decor_face_valid(face)) {
		return null_handle;
	}
	if (start_pos > end_pos) {
		size_t tmp_pos = start_pos;
		enum kg_marker_gravity tmp_grav = start_gravity;

		start_pos = end_pos;
		end_pos = tmp_pos;
		start_gravity = end_gravity;
		end_gravity = tmp_grav;
	}

	if (!b->decorations) {
		b->decorations = decor_alloc_ok()
		    ? calloc(1, sizeof(*b->decorations))
		    : NULL;
		if (!b->decorations) {
			return null_handle;
		}
		b->decorations->next_id = 1;
	}
	store = b->decorations;
	if (store->next_id == UINT32_MAX || !decor_store_reserve(store)) {
		return null_handle;
	}

	start = kg_marker_create(b, start_pos, start_gravity);
	if (start.id == 0) {
		return null_handle;
	}
	end = kg_marker_create(b, end_pos, end_gravity);
	if (end.id == 0) {
		kg_marker_delete(start);
		return null_handle;
	}

	/* `id` never repeats within one store's lifetime (refused rather than
	 * wrapped at exhaustion, above), which already tells a stale handle
	 * from a live record; `generation` exists for the same handle shape
	 * struct kg_marker_handle has, not because this packed, always-
	 * compacted array has a slot identity for it to track. */
	rec = (struct kg_decor) {
		.id = store->next_id++,
		.generation = 1,
		.start = start,
		.end = end,
		.face = face,
		.priority = priority,
		.flags = decor_flags(start_gravity, end_gravity, evaporate),
	};
	idx = decor_insert_index(store, start_pos, end_pos, priority, rec.id);
	memmove(&store->items[idx + 1], &store->items[idx],
	    (store->count - idx) * sizeof(*store->items));
	store->items[idx] = rec;
	store->count++;

	return (
	    struct kg_decor_handle) { start.buffer, rec.id, rec.generation };
}

/* buf_resolve() plus a store lookup, shared by every call that reaches an
 * existing record by handle.  `out_store`/`out_idx` may be NULL for a
 * caller that only wants the record itself. */
static struct kg_decor *decor_lookup(struct kg_decor_handle handle,
    struct kg_decor_store **out_store, size_t *out_idx)
{
	struct editor_buffer *b = buf_resolve(handle.buffer);
	struct kg_decor_store *store;

	if (!b || !b->decorations) {
		return NULL;
	}
	store = b->decorations;
	if (out_store) {
		*out_store = store;
	}
	return decor_store_find(store, handle.id, handle.generation, out_idx);
}

enum kg_decor_result kg_decor_resolve(
    struct kg_decor_handle handle, struct kg_decor_info *out)
{
	struct kg_decor *d = decor_lookup(handle, NULL, NULL);
	size_t s, e;

	if (!d) {
		return KG_DECOR_GONE;
	}
	if (kg_marker_resolve(d->start, &s) != KG_MARKER_OK
	    || kg_marker_resolve(d->end, &e) != KG_MARKER_OK) {
		return KG_DECOR_GONE;
	}
	if (out) {
		*out = (struct kg_decor_info) {
			.start = s,
			.end = e,
			.face = d->face,
			.priority = d->priority,
			.evaporate = (d->flags & KG_DECOR_FLAG_EVAPORATE) != 0,
		};
	}
	return KG_DECOR_OK;
}

bool kg_decor_delete(struct kg_decor_handle handle)
{
	struct kg_decor_store *store;
	size_t idx;
	struct kg_decor *d = decor_lookup(handle, &store, &idx);

	if (!d) {
		return false;
	}
	kg_marker_delete(d->start);
	kg_marker_delete(d->end);
	memmove(&store->items[idx], &store->items[idx + 1],
	    (store->count - idx - 1) * sizeof(*store->items));
	store->count--;
	return true;
}

void kg_decor_compact(struct editor_buffer *b)
{
	struct kg_decor_store *store;
	size_t w = 0;

	if (!b || !b->decorations) {
		return;
	}
	store = b->decorations;
	for (size_t r = 0; r < store->count; r++) {
		struct kg_decor *d = &store->items[r];
		size_t s, e;
		bool start_ok = kg_marker_resolve(d->start, &s) == KG_MARKER_OK;
		bool end_ok = kg_marker_resolve(d->end, &e) == KG_MARKER_OK;
		bool evaporate = (d->flags & KG_DECOR_FLAG_EVAPORATE) != 0;

		if (!start_ok || !end_ok || (evaporate && s == e)) {
			kg_marker_delete(d->start);
			kg_marker_delete(d->end);
			continue;
		}
		if (w != r) {
			store->items[w] = *d;
		}
		w++;
	}
	store->count = w;
}

void kg_decor_query_begin(struct kg_decor_query *q, struct editor_buffer *b,
    size_t range_start, size_t range_end)
{
	*q = (struct kg_decor_query) {
		.store = b ? b->decorations : NULL,
		.range_start = range_start,
		.range_end = range_end,
		.index = 0,
	};
}

bool kg_decor_query_next(
    struct kg_decor_query *q, struct kg_decor_query_span *out)
{
	if (!q->store) {
		return false;
	}
	while (q->index < q->store->count) {
		struct kg_decor *d = &q->store->items[q->index++];
		size_t s, e;

		KG_PERF_INC(KG_PERF_DECOR_EXAMINED);
		if (kg_marker_resolve(d->start, &s) != KG_MARKER_OK
		    || kg_marker_resolve(d->end, &e) != KG_MARKER_OK) {
			continue; /* stale endpoint: gone, not a guessed span */
		}
		if (s >= q->range_end) {
			/* Sorted by resolved start ascending: nothing further
			 * in the store can start before range_end either, so
			 * the walk is over. */
			q->index = q->store->count;
			return false;
		}
		if (e <= q->range_start) {
			continue; /* ends at or before the visible range */
		}
		if (out) {
			*out = (struct kg_decor_query_span) {
				.start = s,
				.end = e,
				.face = d->face,
				.priority = d->priority,
				.evaporate
				= (d->flags & KG_DECOR_FLAG_EVAPORATE) != 0,
				.id = d->id,
			};
		}
		KG_PERF_INC(KG_PERF_DECOR_VISIBLE);
		return true;
	}
	return false;
}

void kg_decor_store_free(struct editor_buffer *b)
{
	if (!b || !b->decorations) {
		return;
	}
	free(b->decorations->items);
	free(b->decorations);
	b->decorations = NULL;
}
