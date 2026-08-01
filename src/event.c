/* event.c — one bounded typed event queue for committed mutations and
 * lifecycle changes.  See event.h for the shape and the producer/consumer
 * contract; nothing here resolves a handle or reaches into
 * struct editor_buffer. */

#include "event.h"
#include "def.h" /* MAX_BUFFERS: the overflow table has one entry per slot */
#include <stdio.h>
#include <string.h>

/* One ring slot: the envelope plus the coalescing token, which is queue
 * bookkeeping and never delivered -- kg_event_queue_pop() copies out only
 * `ev`. */
struct kg_event_ring_slot {
	struct kg_event ev;
	command_id token;
};

/* One buffer's pending broad-change summary: at most one per live buffer,
 * indexed by buffer slot.  A slot whose handle no longer matches the
 * entry stored there belongs to whatever occupies (or occupied) that
 * slot now, not the buffer that made the entry -- see overflow_publish(). */
struct kg_event_overflow_entry {
	bool in_use;
	struct kg_buffer_handle buffer;
	struct kg_event_buffer_extent extent;
	uint64_t generation;
	uint64_t injected_before_seq;
};

static struct kg_event_ring_slot ring[KG_EVENT_RING_MAX];
static size_t ring_head;
static size_t ring_count;
static size_t ring_capacity = KG_EVENT_RING_MAX;
static size_t reserved_outstanding;
static uint64_t next_seq = 1;
static struct kg_event_overflow_entry overflow[MAX_BUFFERS];

/* ---- payload constructors ---------------------------------------------- */

struct kg_event kg_event_make_buffer_changed(struct kg_buffer_handle buffer,
    uint64_t begin_byte, uint64_t old_len, uint64_t new_len,
    uint64_t generation)
{
	struct kg_event ev = { .kind = KG_EVENT_BUFFER_CHANGED };

	ev.payload.changed = (struct kg_event_buffer_changed) {
		.buffer = buffer,
		.begin_byte = begin_byte,
		.old_len = old_len,
		.new_len = new_len,
		.generation = generation,
	};
	return ev;
}

struct kg_event kg_event_make_buffer_broad(struct kg_buffer_handle buffer,
    struct kg_event_buffer_extent extent, uint64_t generation)
{
	struct kg_event ev = { .kind = KG_EVENT_BUFFER_BROAD_CHANGE };

	ev.payload.broad = (struct kg_event_buffer_broad) {
		.buffer = buffer,
		.extent = extent,
		.generation = generation,
	};
	return ev;
}

static struct kg_event make_lifecycle(
    enum kg_event_kind kind, struct kg_buffer_handle buffer)
{
	struct kg_event ev = { .kind = kind };

	ev.payload.buffer_life = (struct kg_event_buffer_lifecycle) {
		.buffer = buffer,
	};
	return ev;
}

struct kg_event kg_event_make_buffer_opened(struct kg_buffer_handle buffer)
{
	return make_lifecycle(KG_EVENT_BUFFER_OPENED, buffer);
}

struct kg_event kg_event_make_buffer_killing(struct kg_buffer_handle buffer)
{
	return make_lifecycle(KG_EVENT_BUFFER_KILLING, buffer);
}

struct kg_event kg_event_make_buffer_killed(struct kg_buffer_handle buffer)
{
	return make_lifecycle(KG_EVENT_BUFFER_KILLED, buffer);
}

static struct kg_event make_view(
    enum kg_event_kind kind, int window_slot, struct kg_buffer_handle buffer)
{
	struct kg_event ev = { .kind = kind };

	ev.payload.view = (struct kg_event_view) {
		.window_slot = window_slot,
		.buffer = buffer,
	};
	return ev;
}

struct kg_event kg_event_make_view_attached(
    int window_slot, struct kg_buffer_handle buffer)
{
	return make_view(KG_EVENT_VIEW_ATTACHED, window_slot, buffer);
}

struct kg_event kg_event_make_view_detached(
    int window_slot, struct kg_buffer_handle buffer)
{
	return make_view(KG_EVENT_VIEW_DETACHED, window_slot, buffer);
}

struct kg_event kg_event_make_before_save(struct kg_buffer_handle buffer)
{
	struct kg_event ev = { .kind = KG_EVENT_BEFORE_SAVE };

	ev.payload.before_save
	    = (struct kg_event_before_save) { .buffer = buffer };
	return ev;
}

struct kg_event kg_event_make_after_save(
    struct kg_buffer_handle buffer, bool success)
{
	struct kg_event ev = { .kind = KG_EVENT_AFTER_SAVE };

	ev.payload.after_save = (struct kg_event_after_save) {
		.buffer = buffer,
		.success = success,
	};
	return ev;
}

struct kg_event kg_event_make_mode_changed(
    struct kg_buffer_handle buffer, const char *mode_name)
{
	struct kg_event ev = { .kind = KG_EVENT_MODE_CHANGED };

	ev.payload.mode_changed.buffer = buffer;
	ev.payload.mode_changed.name[0] = '\0';
	if (mode_name) {
		snprintf(ev.payload.mode_changed.name,
		    sizeof(ev.payload.mode_changed.name), "%s", mode_name);
	}
	return ev;
}

/* ---- ring and overflow bookkeeping -------------------------------------- */

static bool handles_equal(struct kg_buffer_handle a, struct kg_buffer_handle b)
{
	return a.slot == b.slot && a.id == b.id && a.generation == b.generation;
}

static void ring_push(const struct kg_event *ev, command_id token)
{
	size_t idx = (ring_head + ring_count) % ring_capacity;

	ring[idx].ev = *ev;
	ring[idx].token = token;
	ring_count++;
}

/* Slots a droppable change event may use: the rest stay free for
 * kg_event_reserve_lifecycle(). */
static bool droppable_has_room(void)
{
	size_t droppable_cap = ring_capacity > KG_EVENT_LIFECYCLE_RESERVE
	    ? ring_capacity - KG_EVENT_LIFECYCLE_RESERVE
	    : 0;

	return ring_count < droppable_cap;
}

static bool lifecycle_has_room(void)
{
	return ring_count + reserved_outstanding < ring_capacity;
}

/* Record or extend `buffer`'s overflow summary.  A matching in-use entry
 * updates its new extent/generation and keeps its first old extent and
 * injection point; anything else -- no entry, or one belonging to a
 * different identity that occupied this slot before -- starts fresh, so
 * slot reuse never merges one buffer's lengths into another's summary. */
static void overflow_publish(struct kg_buffer_handle buffer,
    struct kg_event_buffer_extent extent, uint64_t generation, uint64_t seq)
{
	struct kg_event_overflow_entry *e;

	if (buffer.slot < 0 || buffer.slot >= MAX_BUFFERS) {
		return;
	}
	e = &overflow[buffer.slot];
	if (e->in_use && handles_equal(e->buffer, buffer)) {
		e->extent.new_total_len = extent.new_total_len;
		e->generation = generation;
		return;
	}
	*e = (struct kg_event_overflow_entry) {
		.in_use = true,
		.buffer = buffer,
		.extent = extent,
		.generation = generation,
		.injected_before_seq = seq,
	};
}

/* Shared tail of kg_event_queue_text_change() and
 * kg_event_queue_broad_change(): place `ev` in the ring if the droppable
 * share has room, else fold `extent`/`generation` into `buffer`'s
 * overflow summary at this call's ordering point. */
static enum kg_event_enqueue_result enqueue_change(struct kg_event ev,
    struct kg_buffer_handle buffer, struct kg_event_buffer_extent extent,
    uint64_t generation, command_id token)
{
	uint64_t seq = next_seq++;

	ev.seq = seq;
	if (droppable_has_room()) {
		ring_push(&ev, token);
		return KG_EVENT_QUEUED_EXACT;
	}
	overflow_publish(buffer, extent, generation, seq);
	return KG_EVENT_QUEUED_BROAD;
}

/* True (with `*out_new_len` set) exactly when the pre-edit span
 * [begin, begin+old_len) sits inside the previous coalesced entry's
 * post-edit span [prev_begin, prev_begin+prev_new_len] -- an edit landing
 * within, or growing from, the tail entry's own new content.  This is the
 * one shape merged exactly here: composing the *old*-length side of a
 * leftward-growing or disjoint pair needs relocating that boundary
 * backward through the earlier edit too, which this module does not
 * attempt -- such a pair is left as two ring entries instead, per
 * event.h's kg_event_queue_text_change() contract. */
static bool coalesce_contained(uint64_t prev_begin, uint64_t prev_new_len,
    uint64_t begin, uint64_t old_len, uint64_t new_len, uint64_t *out_new_len)
{
	uint64_t prev_end, cur_end, shrunk;

	if (begin < prev_begin || prev_new_len > UINT64_MAX - prev_begin) {
		return false;
	}
	prev_end = prev_begin + prev_new_len;
	if (old_len > UINT64_MAX - begin) {
		return false;
	}
	cur_end = begin + old_len;
	if (cur_end > prev_end) {
		return false;
	}
	/* cur_end - begin <= prev_new_len, guaranteed by containment above,
	 * so this cannot underflow. */
	shrunk = prev_new_len - (cur_end - begin);
	if (shrunk > UINT64_MAX - new_len) {
		return false;
	}
	*out_new_len = shrunk + new_len;
	return true;
}

/* Merge into the ring's tail entry when it is an exact change for the
 * same buffer and command token, at the generation right after it, whose
 * post-edit span contains this edit -- see coalesce_contained().  Updates
 * the tail in place and returns true; false leaves the ring untouched for
 * an ordinary enqueue. */
static bool try_coalesce_tail(struct kg_buffer_handle buffer, uint64_t begin,
    uint64_t old_len, uint64_t new_len, uint64_t generation, command_id token)
{
	struct kg_event_ring_slot *tail;
	struct kg_event_buffer_changed *prev;
	uint64_t merged_new_len;

	if (ring_count == 0 || token == CMD_ID_NONE) {
		return false;
	}
	tail = &ring[(ring_head + ring_count - 1) % ring_capacity];
	if (tail->ev.kind != KG_EVENT_BUFFER_CHANGED || tail->token != token) {
		return false;
	}
	prev = &tail->ev.payload.changed;
	if (prev->generation == UINT64_MAX) {
		return false; /* prev->generation + 1 would wrap */
	}
	if (!handles_equal(prev->buffer, buffer)
	    || prev->generation + 1 != generation) {
		return false;
	}
	if (!coalesce_contained(prev->begin_byte, prev->new_len, begin, old_len,
		new_len, &merged_new_len)) {
		return false;
	}
	prev->new_len = merged_new_len;
	prev->generation = generation;
	return true;
}

/* ---- producers ----------------------------------------------------------- */

enum kg_event_enqueue_result kg_event_queue_text_change(
    struct kg_buffer_handle buffer, uint64_t begin_byte, uint64_t old_len,
    uint64_t new_len, struct kg_event_buffer_extent extent, uint64_t generation,
    command_id token)
{
	struct kg_event ev;

	if (try_coalesce_tail(
		buffer, begin_byte, old_len, new_len, generation, token)) {
		return KG_EVENT_QUEUED_EXACT;
	}
	ev = kg_event_make_buffer_changed(
	    buffer, begin_byte, old_len, new_len, generation);
	return enqueue_change(ev, buffer, extent, generation, token);
}

enum kg_event_enqueue_result kg_event_queue_broad_change(
    struct kg_buffer_handle buffer, struct kg_event_buffer_extent extent,
    uint64_t generation)
{
	struct kg_event ev
	    = kg_event_make_buffer_broad(buffer, extent, generation);

	return enqueue_change(ev, buffer, extent, generation, CMD_ID_NONE);
}

struct kg_event_reservation kg_event_reserve_lifecycle(void)
{
	if (!lifecycle_has_room()) {
		return (struct kg_event_reservation) { .valid = false };
	}
	reserved_outstanding++;
	return (struct kg_event_reservation) { .valid = true };
}

void kg_event_release_reservation(struct kg_event_reservation *res)
{
	if (!res || !res->valid) {
		return;
	}
	res->valid = false;
	reserved_outstanding--;
}

enum kg_event_enqueue_result kg_event_publish_lifecycle(
    struct kg_event_reservation *res, struct kg_event ev)
{
	if (!res || !res->valid) {
		return KG_EVENT_REFUSED;
	}
	res->valid = false;
	reserved_outstanding--;
	ev.seq = next_seq++;
	ring_push(&ev, CMD_ID_NONE);
	return KG_EVENT_QUEUED;
}

/* ---- consumer -------------------------------------------------------------
 */

bool kg_event_queue_pop(struct kg_event *out)
{
	int overflow_idx = -1;
	uint64_t overflow_seq = 0;

	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (!overflow[i].in_use) {
			continue;
		}
		if (overflow_idx < 0
		    || overflow[i].injected_before_seq < overflow_seq) {
			overflow_idx = i;
			overflow_seq = overflow[i].injected_before_seq;
		}
	}
	if (overflow_idx >= 0
	    && (ring_count == 0 || overflow_seq <= ring[ring_head].ev.seq)) {
		if (out) {
			*out = kg_event_make_buffer_broad(
			    overflow[overflow_idx].buffer,
			    overflow[overflow_idx].extent,
			    overflow[overflow_idx].generation);
			out->seq = overflow_seq;
		}
		overflow[overflow_idx].in_use = false;
		return true;
	}
	if (ring_count == 0) {
		return false;
	}
	if (out) {
		*out = ring[ring_head].ev;
	}
	ring_head = (ring_head + 1) % ring_capacity;
	ring_count--;
	return true;
}

/* ---- lifecycle of the queue itself --------------------------------------- */

void kg_event_queue_init(void)
{
	ring_head = 0;
	ring_count = 0;
	ring_capacity = KG_EVENT_RING_MAX;
	next_seq = 1;
	reserved_outstanding = 0;
	memset(overflow, 0, sizeof(overflow));
}

void kg_event_queue_set_capacity_for_test(size_t capacity)
{
	kg_event_queue_init();
	if (capacity < 1) {
		capacity = 1;
	}
	if (capacity > KG_EVENT_RING_MAX) {
		capacity = KG_EVENT_RING_MAX;
	}
	ring_capacity = capacity;
}
