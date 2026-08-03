/* event.c — one bounded typed event queue for committed mutations and
 * lifecycle changes.  See event.h for the shape and the producer/consumer
 * contract; nothing here resolves a handle or reaches into
 * struct editor_buffer. */

#include "event.h"
#include "def.h" /* MAX_BUFFERS: the overflow table has one entry per slot */
#include "process_table.h" /* kg_process_table_resolves(): event_resolution() */
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

static struct kg_event make_view(enum kg_event_kind kind,
    struct kg_window_handle window, struct kg_buffer_handle buffer)
{
	struct kg_event ev = { .kind = kind };

	ev.payload.view = (struct kg_event_view) {
		.window = window,
		.buffer = buffer,
	};
	return ev;
}

struct kg_event kg_event_make_view_attached(
    struct kg_window_handle window, struct kg_buffer_handle buffer)
{
	return make_view(KG_EVENT_VIEW_ATTACHED, window, buffer);
}

struct kg_event kg_event_make_view_detached(
    struct kg_window_handle window, struct kg_buffer_handle buffer)
{
	return make_view(KG_EVENT_VIEW_DETACHED, window, buffer);
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

struct kg_event kg_event_make_process_output(
    struct kg_process_handle process, struct kg_buffer_handle buffer)
{
	struct kg_event ev = { .kind = KG_EVENT_PROCESS_OUTPUT };

	ev.payload.process = (struct kg_event_process) {
		.process = process,
		.buffer = buffer,
	};
	return ev;
}

struct kg_event kg_event_make_process_exit(struct kg_process_handle process,
    struct kg_buffer_handle buffer, bool exited, int code)
{
	struct kg_event ev = { .kind = KG_EVENT_PROCESS_EXIT };

	ev.payload.process = (struct kg_event_process) {
		.process = process,
		.buffer = buffer,
		.exited = exited,
		.code = code,
	};
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

/* Shared by kg_event_queue_pop() and kg_event_drain_safe(): pop the next
 * event in publication order -- the ring's front entry, or, when it sorts
 * earlier, a pending overflow summary -- but only when that event's own
 * sequence number is at or below `boundary`.  Otherwise returns false and
 * touches nothing, so a drain's boundary check never has to un-pop what it
 * already took.  `boundary` of UINT64_MAX is "no boundary", which is all
 * kg_event_queue_pop() needs. */
static bool queue_pop_bounded(uint64_t boundary, struct kg_event *out)
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
		if (overflow_seq > boundary) {
			return false;
		}
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
	if (ring_count == 0 || ring[ring_head].ev.seq > boundary) {
		return false;
	}
	if (out) {
		*out = ring[ring_head].ev;
	}
	ring_head = (ring_head + 1) % ring_capacity;
	ring_count--;
	return true;
}

bool kg_event_queue_pop(struct kg_event *out)
{
	return queue_pop_bounded(UINT64_MAX, out);
}

/* ---- lifecycle of the queue itself --------------------------------------- */

/* Forward declaration: the subscriber registry this resets is defined
 * below, but kg_event_queue_init() -- this file's one "back to a known
 * state" entry point, which is all any test's setup()/teardown() calls --
 * clears it too, so a test never leaks a subscription into the next
 * test. */
static void event_subscribers_reset(void);

void kg_event_queue_init(void)
{
	ring_head = 0;
	ring_count = 0;
	ring_capacity = KG_EVENT_RING_MAX;
	next_seq = 1;
	reserved_outstanding = 0;
	memset(overflow, 0, sizeof(overflow));
	event_subscribers_reset();
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

/* ---- Phase 5: subscriber registry ---------------------------------------
 *
 * See event.h for the contract.  `reg_seq` is what makes dispatch order
 * independent of slot reuse: a slot freed by unsubscribe and handed to a
 * later kg_event_subscribe() call gets a fresh, larger reg_seq, so
 * snapshot_subscribers() below sorts the newcomer after every subscriber
 * still active from before -- exactly registration order, not slot
 * order. */
struct kg_event_subscriber {
	kg_event_callback cb;
	void *ctx;
	unsigned kind_mask;
	uint64_t reg_seq;
	uint32_t generation;
	bool active;
};

static struct kg_event_subscriber subs[KG_EVENT_MAX_SUBSCRIBERS];
static uint64_t next_reg_seq = 1;
static uint32_t next_sub_generation = 1;
static unsigned prompt_depth;
static unsigned drain_depth;
static unsigned max_drain_depth_seen;
static unsigned error_count;

static void event_subscribers_reset(void)
{
	memset(subs, 0, sizeof(subs));
	next_reg_seq = 1;
	next_sub_generation = 1;
	prompt_depth = 0;
	drain_depth = 0;
	max_drain_depth_seen = 0;
	error_count = 0;
}

struct kg_event_subscriber_token kg_event_subscribe(
    unsigned kind_mask, kg_event_callback cb, void *ctx)
{
	size_t i;

	if (!cb) {
		return KG_EVENT_SUBSCRIBER_NONE;
	}
	for (i = 0; i < KG_EVENT_MAX_SUBSCRIBERS; i++) {
		if (!subs[i].active) {
			break;
		}
	}
	if (i == KG_EVENT_MAX_SUBSCRIBERS) {
		return KG_EVENT_SUBSCRIBER_NONE;
	}
	subs[i] = (struct kg_event_subscriber) {
		.cb = cb,
		.ctx = ctx,
		.kind_mask = kind_mask,
		.reg_seq = next_reg_seq++,
		.generation = next_sub_generation++,
		.active = true,
	};
	return (struct kg_event_subscriber_token) {
		.slot = (uint32_t)i,
		.generation = subs[i].generation,
	};
}

bool kg_event_unsubscribe(struct kg_event_subscriber_token token)
{
	struct kg_event_subscriber *s;

	if (token.slot >= KG_EVENT_MAX_SUBSCRIBERS) {
		return false;
	}
	s = &subs[token.slot];
	if (!s->active || s->generation != token.generation) {
		return false;
	}
	s->active = false;
	return true;
}

void kg_event_prompt_enter(void) { prompt_depth++; }

void kg_event_prompt_leave(void)
{
	if (prompt_depth > 0) {
		prompt_depth--;
	}
}

/* One entry of a drain's subscriber snapshot: which slot, and the
 * generation it held when the drain started.  Re-checked against the live
 * registry before every call, so an unsubscribe -- another subscriber's,
 * or its own, from a callback earlier in the same drain -- is honoured
 * immediately rather than at the next drain. */
struct kg_event_sub_snapshot {
	uint32_t slot;
	uint32_t generation;
};

/* Every currently active subscriber, insertion-sorted by registration
 * order (`reg_seq`) rather than slot index, which slot reuse can put out
 * of order.  KG_EVENT_MAX_SUBSCRIBERS is small enough that an insertion
 * sort costs nothing worth avoiding it for. */
static size_t snapshot_subscribers(struct kg_event_sub_snapshot *out)
{
	size_t n = 0, i, j;

	for (i = 0; i < KG_EVENT_MAX_SUBSCRIBERS; i++) {
		if (subs[i].active) {
			out[n].slot = (uint32_t)i;
			out[n].generation = subs[i].generation;
			n++;
		}
	}
	for (i = 1; i < n; i++) {
		struct kg_event_sub_snapshot key = out[i];
		uint64_t key_seq = subs[key.slot].reg_seq;

		j = i;
		while (j > 0 && subs[out[j - 1].slot].reg_seq > key_seq) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = key;
	}
	return n;
}

/* Which buffer handle `ev` carries, whatever its kind -- the one thing
 * every payload but broad-change-free kinds has.  Used only to re-resolve
 * at delivery; nothing here reads any other field. */
static struct kg_buffer_handle event_buffer_handle(const struct kg_event *ev)
{
	switch (ev->kind) {
	case KG_EVENT_BUFFER_CHANGED:
		return ev->payload.changed.buffer;
	case KG_EVENT_BUFFER_BROAD_CHANGE:
		return ev->payload.broad.buffer;
	case KG_EVENT_BUFFER_OPENED:
	case KG_EVENT_BUFFER_KILLING:
	case KG_EVENT_BUFFER_KILLED:
		return ev->payload.buffer_life.buffer;
	case KG_EVENT_VIEW_ATTACHED:
	case KG_EVENT_VIEW_DETACHED:
		return ev->payload.view.buffer;
	case KG_EVENT_BEFORE_SAVE:
		return ev->payload.before_save.buffer;
	case KG_EVENT_AFTER_SAVE:
		return ev->payload.after_save.buffer;
	case KG_EVENT_MODE_CHANGED:
		return ev->payload.mode_changed.buffer;
	case KG_EVENT_PROCESS_OUTPUT:
	case KG_EVENT_PROCESS_EXIT:
		/* Carried for the drain subscriber, but not what these two
		 * kinds resolve on -- see event_resolution(). */
		return ev->payload.process.buffer;
	}
	return (struct kg_buffer_handle) { 0 };
}

/* Whether `ev` resolves, as a bool -- event_resolution() below is just this
 * cast to the enum, kept separate so its own complexity stays where it was
 * before this had two identities to tell apart instead of one.  For every
 * buffer-identified kind this is buf_resolve() on the handle
 * event_buffer_handle() reads out, the same call a live caller would make.
 * A process event resolves on its *process* handle instead
 * (kg_process_table_resolves(), src/process_table.h): that is the identity
 * the drain subscriber dispatches on, and a KG_EVENT_PROCESS_EXIT for a
 * since-released slot still resolves false, delivered as
 * KG_EVENT_RESOLVED_GONE the same way a KG_EVENT_BUFFER_KILLED is. */
static bool event_resolves(const struct kg_event *ev)
{
	if (ev->kind == KG_EVENT_PROCESS_OUTPUT
	    || ev->kind == KG_EVENT_PROCESS_EXIT) {
		return kg_process_table_resolves(ev->payload.process.process);
	}
	return buf_resolve(event_buffer_handle(ev)) != NULL;
}

static enum kg_event_resolution event_resolution(const struct kg_event *ev)
{
	return event_resolves(ev) ? KG_EVENT_RESOLVED_LIVE
				  : KG_EVENT_RESOLVED_GONE;
}

/* Run `ev` past every subscriber in `snap`, in the snapshot's order.  Each
 * slot is re-checked live before its call: a subscriber unsubscribed
 * earlier in this same drain (by itself or another callback) is skipped,
 * not called on a stale pointer, and a kind_mask that does not name `ev`'s
 * kind skips it too.  An UNSUBSCRIBE return retires the slot for every
 * later event this drain and beyond; an ERROR return is counted and never
 * stops the loop. */
static void dispatch_event(const struct kg_event *ev,
    const struct kg_event_sub_snapshot *snap, size_t count)
{
	enum kg_event_resolution res = event_resolution(ev);
	size_t i;

	for (i = 0; i < count; i++) {
		struct kg_event_subscriber *s = &subs[snap[i].slot];
		enum kg_event_cb_status status;

		if (!s->active || s->generation != snap[i].generation) {
			continue;
		}
		if (!(s->kind_mask & (1u << ev->kind))) {
			continue;
		}
		status = s->cb(ev, res, s->ctx);
		if (status == KG_EVENT_CB_UNSUBSCRIBE) {
			s->active = false;
		} else if (status == KG_EVENT_CB_ERROR) {
			error_count++;
		}
	}
}

#if KG_DEBUG_STATE
static unsigned debug_unsafe_flags;

void kg_event_debug_enter(enum kg_event_unsafe_reason reason)
{
	debug_unsafe_flags |= (unsigned)reason;
}

void kg_event_debug_leave(enum kg_event_unsafe_reason reason)
{
	debug_unsafe_flags &= ~(unsigned)reason;
}

/* Abort, naming which reasons, rather than deliver into a half-finished
 * edit or a half-painted frame -- the three safe points this is meant to
 * be called from never have any of these bits set, so seeing one here
 * means a call site outside them. */
static void assert_safe_to_drain(void)
{
	if (debug_unsafe_flags == 0) {
		return;
	}
	fprintf(stderr,
	    "kg: kg_event_drain_safe() called outside a safe point "
	    "(reason flags=0x%x)\n",
	    debug_unsafe_flags);
	abort();
}
#else
static void assert_safe_to_drain(void) { }
#endif

void kg_event_drain_safe(void)
{
	uint64_t boundary;
	struct kg_event ev;
	struct kg_event_sub_snapshot snap[KG_EVENT_MAX_SUBSCRIBERS];
	size_t count;

	if (prompt_depth > 0 || drain_depth > 0) {
		return;
	}
	assert_safe_to_drain();
	drain_depth++;
	if (drain_depth > max_drain_depth_seen) {
		max_drain_depth_seen = drain_depth;
	}
	boundary = next_seq > 0 ? next_seq - 1 : 0;
	count = snapshot_subscribers(snap);
	while (queue_pop_bounded(boundary, &ev)) {
		dispatch_event(&ev, snap, count);
	}
	drain_depth--;
}

unsigned kg_event_test_max_drain_depth(void) { return max_drain_depth_seen; }

unsigned kg_event_test_error_count(void) { return error_count; }
