/* event.h — one bounded typed event queue for committed mutations and
 * lifecycle changes.
 *
 * An envelope is a tag plus a fixed-size payload: a handle, a length, a
 * generation, a flag, or (only where a consumer demonstrably needs it) a
 * bounded copied name.  Never a borrowed row pointer, a filename, row
 * text, a decoration pointer, or any other runtime value -- see
 * src/marker.h and src/decor.h for the same discipline applied to
 * positions and spans.  This module does not resolve the handles it is
 * given (no buf_resolve(), no dependency on struct editor_buffer at all):
 * it stores and orders what a producer publishes, and a consumer
 * re-resolves each handle at delivery time, which is follow-up Plan 03
 * Phase 5's job, not this one's.
 *
 * Storage is two fixed-capacity pieces, both owned by this module and
 * neither ever grown: a ring of exact and lifecycle events, and a
 * `MAX_BUFFERS`-entry overflow table holding at most one pending
 * broad-change summary per live buffer.  Nothing here calls the
 * allocator.
 *
 * Two producer disciplines, matching the plan's split:
 *
 *   - Change events (KG_EVENT_BUFFER_CHANGED, KG_EVENT_BUFFER_BROAD_CHANGE)
 *     never refuse.  kg_event_queue_text_change() and
 *     kg_event_queue_broad_change() either place an exact event in the
 *     ring or, under ring pressure, collapse it into that buffer's
 *     overflow summary -- precision is what queue pressure may cost a
 *     text edit, never the edit itself.
 *
 *   - Lifecycle events (open/kill/view/save/mode) reserve capacity before
 *     the transition they describe: kg_event_reserve_lifecycle() first,
 *     then kg_event_publish_lifecycle() once the transition has happened,
 *     or kg_event_release_reservation() if it did not.  A refused
 *     reservation means the caller's old state is still intact -- queue
 *     pressure may never silently drop a kill, attach or save transition
 *     no subscriber can observe.
 *
 * Producers are wired throughout src/ (buffer.c, bufmgr.c, fileio.c,
 * syntax.c).  The rest of this header is Phase 5: a small subscriber
 * registry plus kg_event_drain_safe(), the only thing that ever calls
 * kg_event_queue_pop() outside a test.  Delivery happens only from the
 * named safe points in src/main.c's loop -- never from inside a producer,
 * a command handler, or a prompt -- so a callback never sees a partial
 * edit or a screen mid-repaint. */

#ifndef KG_EVENT_H
#define KG_EVENT_H

#include "bufhandle.h"
#include "cmd.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The physical size of the ring, never grown.  A test shrinks the
 * *logical* capacity below this with kg_event_queue_set_capacity_for_test()
 * to exercise wraparound and overflow without a 64-entry fixture; nothing
 * ever asks for more than this many physical slots. */
#define KG_EVENT_RING_MAX 64

/* Ring slots a droppable (change) event may never claim, kept free so a
 * lifecycle reservation still has somewhere to go under heavy edit
 * traffic.  Text edits do not queue for one of these slots; they overflow
 * into the buffer's broad-change summary instead once the ring's
 * droppable share (capacity - this) is full. */
#define KG_EVENT_LIFECYCLE_RESERVE 4

/* A mode name is a short display string ("C", "Python", "Fundamental",
 * "Lisp Interaction"); see src/syntax.h's struct editor_syntax::name.
 * There is no numeric mode identity to carry instead -- the syntax
 * pointer itself is a runtime value this queue must not store -- so a
 * mode-changed subscriber's only way to know which mode is this bounded
 * copy of the name. */
#define KG_EVENT_MODE_NAME_MAX 32

enum kg_event_kind {
	KG_EVENT_BUFFER_CHANGED = 0,
	KG_EVENT_BUFFER_BROAD_CHANGE,
	KG_EVENT_BUFFER_OPENED,
	KG_EVENT_BUFFER_KILLING, /* about to be killed; handle still resolves */
	KG_EVENT_BUFFER_KILLED, /* cleanup done; handle is identity evidence
				   only */
	KG_EVENT_VIEW_ATTACHED,
	KG_EVENT_VIEW_DETACHED,
	KG_EVENT_BEFORE_SAVE,
	KG_EVENT_AFTER_SAVE,
	KG_EVENT_MODE_CHANGED,
	/* Process output/exit arrive through this same envelope once Plan 06
	 * adds process descriptors; there is no producer or payload for them
	 * yet, and no kind reserved for them here -- adding one is that
	 * slice's, not this one's, to keep the union's cost off this
	 * module's budget until it is needed. */
};

/* An exact text change: the buffer, the byte where the replaced span
 * started, that span's old and new lengths (not the buffer's total
 * length -- see struct kg_event_buffer_extent for that), and the content
 * generation the change produced.  Queued only after byte and
 * marker/decor publication succeeded; a refused, failed, read-only or
 * byte-identical edit queues nothing (a producer simply does not call
 * kg_event_queue_text_change() for one). */
struct kg_event_buffer_changed {
	struct kg_buffer_handle buffer;
	uint64_t begin_byte;
	uint64_t old_len; /* replaced span's length before the edit */
	uint64_t new_len; /* replaced span's length after the edit */
	uint64_t generation;
};

/* A buffer's total byte length before and after a broad change -- reload,
 * row adoption, or an overflow summary standing in for a run of exact
 * changes this module could not afford to keep precise. */
struct kg_event_buffer_extent {
	uint64_t old_total_len;
	uint64_t new_total_len;
};

struct kg_event_buffer_broad {
	struct kg_buffer_handle buffer;
	struct kg_event_buffer_extent extent;
	uint64_t generation;
};

/* Shared shape for the three lifecycle events that are nothing but an
 * identity: opened, about-to-kill, killed.  killed's handle is kept only
 * as evidence of which identity died; it will not resolve. */
struct kg_event_buffer_lifecycle {
	struct kg_buffer_handle buffer;
};

/* A window's attach/detach to a buffer, addressed by the fields that
 * exist today.  struct editor_window (def.h) has no identity of its own
 * yet -- only the buffer handle it shows -- so `window_slot` is a raw
 * winlist[] index, not a generation-checked handle, and is only as
 * stable as that slot's occupancy.  Follow-up Plan 04's window-handle
 * work widens this payload to a real handle; that is an additive change
 * to this struct, not a second, temporary window registry, which rule 3
 * of the plan's implementation rules forbids. */
struct kg_event_view {
	int window_slot;
	struct kg_buffer_handle buffer;
};

struct kg_event_before_save {
	struct kg_buffer_handle buffer;
};

/* A failed save never pretends the buffer's on-disk state changed;
 * `success` is the whole of what a subscriber is told beyond identity. */
struct kg_event_after_save {
	struct kg_buffer_handle buffer;
	bool success;
};

struct kg_event_mode_changed {
	struct kg_buffer_handle buffer;
	char name[KG_EVENT_MODE_NAME_MAX];
};

union kg_event_payload {
	struct kg_event_buffer_changed changed;
	struct kg_event_buffer_broad broad;
	struct kg_event_buffer_lifecycle buffer_life;
	struct kg_event_view view;
	struct kg_event_before_save before_save;
	struct kg_event_after_save after_save;
	struct kg_event_mode_changed mode_changed;
};

/* One envelope.  `seq` is this module's own publication-order counter --
 * strictly increasing across every publish attempt that produced
 * something (an exact event, a collapsed overflow summary, or a
 * lifecycle event), so a drain can later express "everything up to here"
 * as one comparison.  It is not itself a payload field a consumer reads
 * for meaning, only an ordering key. */
struct kg_event {
	enum kg_event_kind kind;
	uint64_t seq;
	union kg_event_payload payload;
};

/* Every payload constructor.  Each just fills `kind` and the matching
 * union member -- `seq` is 0 until a queue call assigns it -- so a test,
 * or a future producer, can build one and inspect it without touching
 * the queue at all. */
struct kg_event kg_event_make_buffer_changed(struct kg_buffer_handle buffer,
    uint64_t begin_byte, uint64_t old_len, uint64_t new_len,
    uint64_t generation);
struct kg_event kg_event_make_buffer_broad(struct kg_buffer_handle buffer,
    struct kg_event_buffer_extent extent, uint64_t generation);
struct kg_event kg_event_make_buffer_opened(struct kg_buffer_handle buffer);
struct kg_event kg_event_make_buffer_killing(struct kg_buffer_handle buffer);
struct kg_event kg_event_make_buffer_killed(struct kg_buffer_handle buffer);
struct kg_event kg_event_make_view_attached(
    int window_slot, struct kg_buffer_handle buffer);
struct kg_event kg_event_make_view_detached(
    int window_slot, struct kg_buffer_handle buffer);
struct kg_event kg_event_make_before_save(struct kg_buffer_handle buffer);
struct kg_event kg_event_make_after_save(
    struct kg_buffer_handle buffer, bool success);
/* `mode_name` may be NULL or longer than KG_EVENT_MODE_NAME_MAX - 1; NULL
 * leaves the name empty, and a longer name is truncated -- this is a
 * display label, not an identity a consumer resolves by. */
struct kg_event kg_event_make_mode_changed(
    struct kg_buffer_handle buffer, const char *mode_name);

enum kg_event_enqueue_result {
	KG_EVENT_QUEUED_EXACT, /* change event, entered the ring as itself */
	KG_EVENT_QUEUED_BROAD, /* change event, collapsed into the buffer's
				* overflow summary */
	KG_EVENT_QUEUED, /* lifecycle event, entered the ring */
	KG_EVENT_REFUSED, /* lifecycle event: no reserved capacity, or the
			   * reservation handed to kg_event_publish_lifecycle()
			   * was not a live one; nothing was queued */
};

/* Queue one exact text change, or fold it into `buffer`'s overflow
 * summary if the ring's droppable share has no room.  `old_len`/`new_len`
 * describe the replaced span at `begin_byte`; `extent` is the buffer's
 * total length before/after, used only if this collapses into the
 * overflow summary.  Coalesces with the immediately preceding ring entry
 * when it is an exact change for the same buffer and `token`, at an
 * adjacent generation, and `token`'s edit is entirely contained in the
 * tail entry's post-edit span (the common case: successive edits at or
 * growing from one point, e.g. self-insert-command) -- always returning
 * KG_EVENT_QUEUED_EXACT for the merge, since it does not cost the ring an
 * entry.  Anything the merge is not exact and overflow-safe for (a
 * leftward-growing or disjoint edit) is left as two ring entries rather
 * than guessed at; see event.c's coalesce_contained() for exactly which
 * shape is merged.  `token` is CMD_ID_NONE for a producer with no
 * top-level command identity available, which never coalesces.  Never
 * refuses. */
enum kg_event_enqueue_result kg_event_queue_text_change(
    struct kg_buffer_handle buffer, uint64_t begin_byte, uint64_t old_len,
    uint64_t new_len, struct kg_event_buffer_extent extent, uint64_t generation,
    command_id token);

/* Queue one broad-replacement event (reload, row adoption), or fold it
 * into `buffer`'s overflow summary under the same ring pressure rule
 * kg_event_queue_text_change() follows.  Never coalesces with a
 * preceding text change -- a broad change ends any coalescing run for
 * this buffer, since a later exact event's tail check only ever matches
 * another KG_EVENT_BUFFER_CHANGED entry.  Never refuses. */
enum kg_event_enqueue_result kg_event_queue_broad_change(
    struct kg_buffer_handle buffer, struct kg_event_buffer_extent extent,
    uint64_t generation);

/* A credit for exactly one upcoming lifecycle publish, taken before the
 * transition it describes so a refusal leaves the caller's old state
 * untouched.  Zero-initialized is a valid, unreserved value. */
struct kg_event_reservation {
	bool valid;
};

/* Reserve capacity for one lifecycle event.  Fails (returns a value with
 * `valid` false) only when the ring has no room left for it -- see
 * KG_EVENT_LIFECYCLE_RESERVE; a droppable change event can never crowd
 * this out, only other outstanding lifecycle reservations and
 * not-yet-drained lifecycle events can. */
struct kg_event_reservation kg_event_reserve_lifecycle(void);

/* Spend `*res`, publishing `ev` (built by one of the lifecycle
 * constructors above) into the ring.  Returns KG_EVENT_REFUSED without
 * publishing anything when `*res` is not a live reservation -- this is a
 * caller-contract check, not a defense against a stale generation the
 * way marker/decor handles are, since a reservation is never looked up
 * by a stored token, only held by the one caller that took it. */
enum kg_event_enqueue_result kg_event_publish_lifecycle(
    struct kg_event_reservation *res, struct kg_event ev);

/* Give back a reservation the caller decided not to spend -- the
 * transition it was guarding did not happen, for a reason unrelated to
 * this queue.  A no-op on an already-spent or already-released
 * reservation. */
void kg_event_release_reservation(struct kg_event_reservation *res);

/* Pop the next event in publication order: the ring's front entry, or --
 * when it sorts earlier -- a pending overflow summary, injected at the
 * sequence number of the first exact event it replaced rather than
 * appended after whatever the ring holds now.  Returns false with `*out`
 * untouched once both are empty.  `out` may be NULL to just discard the
 * next event. */
bool kg_event_queue_pop(struct kg_event *out);

/* Drop every queued and reserved event, reset sequencing, and restore the
 * full KG_EVENT_RING_MAX capacity.  Also what a test's setup() calls
 * before kg_event_queue_set_capacity_for_test() to start from a known
 * state. */
void kg_event_queue_init(void);

/* Test-only seam: reset the queue (as kg_event_queue_init() does) and set
 * its logical capacity to `capacity`, clamped to [1, KG_EVENT_RING_MAX].
 * Lets a test exercise ring wraparound and overflow collapse without a
 * KG_EVENT_RING_MAX-sized fixture.  Nothing outside test/ calls this --
 * the editor always runs at the full physical capacity, matching
 * kg_decor_fail_alloc_after()'s precedent in decor.h. */
void kg_event_queue_set_capacity_for_test(size_t capacity);

/* ---- Phase 5: subscriber registry and safe-point delivery --------------
 *
 * A subscriber is a C function pointer plus an opaque context and a mask
 * of the event kinds it wants; dispatch runs subscribers in registration
 * order.  kg_event_drain_safe() is the only caller of
 * kg_event_queue_pop()'s machinery from outside a test: it goes only at
 * the three named safe points in src/main.c's loop (after a command and
 * its edit group, after a completed non-command lifecycle action, and
 * before the next screen repaint), because that is the only place none of
 * an edit transaction, the renderer or a prompt read can be mid-way
 * through. */

/* What a callback answers.  KG_EVENT_CB_ERROR is recorded and does not
 * stop later subscribers from running this drain or any later one;
 * nothing here ever longjmps through the loop. */
enum kg_event_cb_status {
	KG_EVENT_CB_CONTINUE = 0,
	KG_EVENT_CB_UNSUBSCRIBE,
	KG_EVENT_CB_ERROR,
};

/* Whether the event's buffer handle still resolves right now -- re-checked
 * at delivery, never the state at publish time.  KG_EVENT_RESOLVED_GONE is
 * expected for KG_EVENT_BUFFER_KILLED and possible for anything else: a
 * dead handle is still delivered, so a subscriber can discard cached
 * state, and no dispatcher hands back the current occupant of a reused
 * slot -- buf_resolve() already refuses that. */
enum kg_event_resolution {
	KG_EVENT_RESOLVED_LIVE,
	KG_EVENT_RESOLVED_GONE,
};

typedef enum kg_event_cb_status (*kg_event_callback)(
    const struct kg_event *ev, enum kg_event_resolution resolution, void *ctx);

#define KG_EVENT_MAX_SUBSCRIBERS 16

/* A registration receipt.  `generation` is what stops a stale token --
 * held past its own unregister, or across the slot's reuse by a later
 * subscriber -- from ever unregistering whoever occupies the slot now.
 * The zero value (KG_EVENT_SUBSCRIBER_NONE) names no subscriber, matching
 * a zeroed struct kg_buffer_handle's convention; kg_event_subscribe()
 * returns it when the registry is full. */
struct kg_event_subscriber_token {
	uint32_t slot;
	uint32_t generation;
};

#define KG_EVENT_SUBSCRIBER_NONE ((struct kg_event_subscriber_token) { 0, 0 })

/* Register `cb`/`ctx` for delivery of every kind set in `kind_mask` (an OR
 * of `1u << KG_EVENT_...` bits).  Returns KG_EVENT_SUBSCRIBER_NONE, having
 * registered nothing, when `cb` is NULL or the registry is already at
 * KG_EVENT_MAX_SUBSCRIBERS.  A subscriber registered while a drain is in
 * progress (from a callback) is not eligible for delivery until the next
 * drain -- kg_event_drain_safe() snapshots who is active once, at the
 * start. */
struct kg_event_subscriber_token kg_event_subscribe(
    unsigned kind_mask, kg_event_callback cb, void *ctx);

/* Remove `token`'s subscriber.  Idempotent: a token already unregistered,
 * never valid, or naming a slot some later registration has since reused
 * answers false and touches nothing -- it never removes the wrong
 * subscriber. */
bool kg_event_unsubscribe(struct kg_event_subscriber_token token);

/* Mark one prompt/minibuffer read as open (nestable: a prompt started from
 * inside another just adds depth).  While any are open,
 * kg_event_drain_safe() defers instead of draining -- eligibility resumes
 * once the outermost kg_event_prompt_leave() runs, not by draining from a
 * prompt's own cleanup. */
void kg_event_prompt_enter(void);
void kg_event_prompt_leave(void);

/* Deliver every event published at or before this call's own start --
 * captured as the queue's latest sequence number right then -- to every
 * subscriber whose mask matches, in registration order.  Events an
 * invoked callback queues (directly, or by editing/killing/saving) are
 * strictly newer than that boundary and wait for the next call instead of
 * being drained in this one; that is what keeps a callback's own work
 * from recursing into a second dispatch of itself.  A no-op while a
 * prompt is open (see kg_event_prompt_enter()) or while a drain is
 * already running.  In a KG_DEBUG_STATE build, also aborts, naming the
 * reason, if called while an edit transaction or the renderer is active
 * -- both are supposed to be impossible at any of the three safe points
 * this is meant to be called from. */
void kg_event_drain_safe(void);

/* Test-only: the deepest kg_event_drain_safe() has ever recursed in this
 * process.  Must never exceed 1; see test/test_event.c. */
unsigned kg_event_test_max_drain_depth(void);

/* Test-only: how many KG_EVENT_CB_ERROR returns kg_event_drain_safe() has
 * recorded since the last kg_event_queue_init(). */
unsigned kg_event_test_error_count(void);

/* Reasons kg_event_drain_safe() refuses to run in a KG_DEBUG_STATE build;
 * see src/buffer.c's kg_buffer_replace() and src/display.c's
 * editor_refresh_screen() for where each is marked.  Not a Fe type and not
 * gated on KG_USE_LISP -- this is core reentrancy bookkeeping, always
 * compiled, just inert (KG_EVENT_DEBUG_ENTER()/_LEAVE() below expand to
 * nothing) unless KG_DEBUG_STATE arms it, matching KG_STATE_CHECK's own
 * precedent in bufhandle.h. */
enum kg_event_unsafe_reason {
	KG_EVENT_UNSAFE_EDIT = 1u << 0,
	KG_EVENT_UNSAFE_RENDER = 1u << 1,
};

#if KG_DEBUG_STATE
void kg_event_debug_enter(enum kg_event_unsafe_reason reason);
void kg_event_debug_leave(enum kg_event_unsafe_reason reason);
#define KG_EVENT_DEBUG_ENTER(reason) kg_event_debug_enter(reason)
#define KG_EVENT_DEBUG_LEAVE(reason) kg_event_debug_leave(reason)
#else
#define KG_EVENT_DEBUG_ENTER(reason) ((void)0)
#define KG_EVENT_DEBUG_LEAVE(reason) ((void)0)
#endif

#endif /* KG_EVENT_H */
