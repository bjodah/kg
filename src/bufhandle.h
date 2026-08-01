#ifndef KG_BUFHANDLE_H
#define KG_BUFHANDLE_H

#include <stddef.h>
#include <stdint.h>

/* How a buffer is named, and how a view says which one it shows.
 *
 * This is the seam between the buffer table and everything that outlives a
 * single command.  It is its own header because `struct editor_window`
 * stores a handle by value and is defined before the buffer table is, so
 * def.h needs the type before it needs either record.
 *
 * Forward declarations only: the records live in def.h, and a consumer that
 * dereferences one includes def.h anyway.  Keeping them out is what lets
 * this header compile on its own (`make header-check`).
 */
struct editor_buffer;
struct editor_window;

/* ---- The identity check both handle families share ---------------------
 *
 * A buffer handle and a window handle answer the same question -- "is slot
 * N still the record I mean?" -- against two different tables.  Addressed
 * by layout (an offset into the record) instead of duplicated per family,
 * so a second handle family costs a few call sites, not a second copy of
 * buf_handle()/buf_resolve()'s logic.  Every record used here is a fixed
 * array element with an `int active` flag, a `uint64_t id` and a
 * `uint64_t generation`, in whatever field order the struct actually
 * declares -- kg_slot_table names where each one is instead of assuming
 * one.  bufmgr.c and winmgr.c are the only definers and the only callers;
 * nothing outside them names struct kg_slot_table. */
struct kg_slot_table {
	void *base;
	int count;
	size_t stride;
	size_t off_active;
	size_t off_id;
	size_t off_generation;
};

enum kg_slot_check { KG_SLOT_OK, KG_SLOT_UNNAMED, KG_SLOT_STALE };

static inline void *kg_slot_rec(struct kg_slot_table t, int slot)
{
	return (char *)t.base + (size_t)slot * t.stride;
}

/* Whether `slot` is in range and its record is active -- the question
 * buf_handle()/win_handle() ask before reading a slot's own identity out
 * of it. */
static inline int kg_slot_active_at(struct kg_slot_table t, int slot)
{
	if (slot < 0 || slot >= t.count) {
		return 0;
	}
	return *(int *)((char *)kg_slot_rec(t, slot) + t.off_active);
}

/* The full identity check buf_resolve()/win_resolve() answer with:
 * KG_SLOT_UNNAMED for a handle that never named anything (out of range, or
 * a zeroed id), KG_SLOT_STALE for one that once looked live and no longer
 * matches (the case a caller may want to count), KG_SLOT_OK once slot,
 * active, id and generation all agree. */
static inline enum kg_slot_check kg_slot_resolves(
    struct kg_slot_table t, int slot, uint64_t id, uint64_t generation)
{
	void *rec;

	if (slot < 0 || slot >= t.count || id == 0) {
		return KG_SLOT_UNNAMED;
	}
	rec = kg_slot_rec(t, slot);
	if (*(int *)((char *)rec + t.off_active)
	    && *(uint64_t *)((char *)rec + t.off_id) == id
	    && *(uint64_t *)((char *)rec + t.off_generation) == generation) {
		return KG_SLOT_OK;
	}
	return KG_SLOT_STALE;
}

/* The slot `rec` sits at, for buf_handle_of()/win_handle_of(), which are
 * handed a record pointer instead of an index.  -1 for NULL or for a
 * pointer outside the table. */
static inline int kg_slot_find(struct kg_slot_table t, const void *rec)
{
	int i;

	for (i = 0; rec && i < t.count; i++) {
		if (rec == kg_slot_rec(t, i)) {
			return i;
		}
	}
	return -1;
}

/* A reference to a buffer that outlives the command that took it.
 *
 * A bare slot index does not: buf_kill() frees the slot and the next open
 * hands it to an unrelated buffer, so an index kept across commands can
 * quietly start naming somebody else's text.  The identity pair survives
 * that, because buf_resolve() refuses to answer unless the slot still holds
 * the same (id, generation) the handle was taken from.  A zeroed handle
 * names nothing. */
struct kg_buffer_handle {
	int slot;
	uint64_t id;
	uint64_t generation;
};

/* The handle naming the buffer in `slot`, or a handle naming nothing. */
struct kg_buffer_handle buf_handle(int slot);

/* The handle naming `b`, for a module that was handed a buffer pointer
 * rather than a slot.  A buffer is not asked to carry its own handle --
 * that would be a second copy of the identity buf_handle() already
 * derives -- so this searches the table for the slot `b` is, and answers
 * a handle naming nothing for NULL or for storage outside buflist[]. */
struct kg_buffer_handle buf_handle_of(const struct editor_buffer *b);

/* The buffer `handle` names, or NULL once it is gone. */
struct editor_buffer *buf_resolve(struct kg_buffer_handle handle);

/* The slot `handle` names, or -1.  Never index an array with the return
 * value without testing it: -1 is the answer for a handle that has died. */
int buf_handle_slot(struct kg_buffer_handle handle);

/* ---- A window's own identity ----
 *
 * Distinct from struct kg_buffer_handle, which names the buffer a window
 * shows: this names the window itself, so an event payload or any other
 * state that outlives a command can say *which window* without keeping a
 * raw winlist[] index.  Same shape and the same exhaustion policy as a
 * buffer handle (see buf_next_id's comment in bufmgr.c); winmgr.c is where
 * a window's generation is claimed, since split/cycle/delete already own
 * the window table. */
struct kg_window_handle {
	int slot;
	uint64_t id;
	uint64_t generation;
};

/* The handle naming the window in `slot`, or a handle naming nothing. */
struct kg_window_handle win_handle(int slot);

/* The handle naming `w`, for a module that was handed a window pointer
 * rather than a slot. */
struct kg_window_handle win_handle_of(const struct editor_window *w);

/* The window `handle` names, or NULL once it is gone. */
struct editor_window *win_resolve(struct kg_window_handle handle);

/* The slot `handle` names, or -1.  Never index winlist[] with the return
 * value without testing it. */
int win_handle_slot(struct kg_window_handle handle);

/* ---- A window's view of a buffer ----
 *
 * A window names its buffer by handle, never by slot.  These four are the
 * whole supported vocabulary; they live in bufmgr.c beside the identity
 * they check. */

/* The buffer `w` shows, or NULL when it shows none.  An inactive window, a
 * detached view and a window left on a killed buffer all answer NULL. */
struct editor_buffer *win_buffer(const struct editor_window *w);

/* The slot `w` shows, or -1.  The only supported way to get an index out
 * of a window, and the reason a failed resolution never becomes one. */
int win_buffer_slot(const struct editor_window *w);

/* Whether `w` shows the buffer `handle` names.  A handle that no longer
 * resolves matches nothing, on either side: two views of nothing are not
 * views of the same thing. */
int win_shows_buffer(
    const struct editor_window *w, struct kg_buffer_handle handle);

/* Hand the buffer a window is leaving the point that window had in it. */
void buf_remember_view(const struct editor_window *w);

/* Point `w` at buffer slot `slot`, resuming where that buffer was last
 * left.  A slot holding no live buffer leaves `w` alone. */
void buf_attach_view(struct editor_window *w, int slot);

/* Take `w` off whatever it shows, banking its point first.  The view is
 * left naming nothing. */
void buf_detach_view(struct editor_window *w);

/* Put every active window back on a live buffer.  Called from the top of
 * the main loop, before the repaint: no window is ever drawn from a handle
 * that has stopped resolving.  Implemented in winmgr.c, which owns the
 * window table. */
void win_check_handles(void);

/* ---- Buffer/view state invariants ----
 *
 * Off unless the build asks for them, the same shape as KG_PERF_COUNTERS
 * and KG_DEBUG_COORDS: the shipped editor carries none of it, and
 * .ci/ci-04 arms them for the sanitizer lane, which drives the whole PTY
 * suite.  Build by hand with
 * `make CFLAGS="-Wall -g -DKG_DEBUG_STATE=1"`.
 *
 * KG_STATE_CHECK() belongs at a lifecycle commit -- a point where the
 * buffer table, the window table and the selection are all supposed to
 * agree -- and nowhere in the middle of one.  It aborts naming the
 * failed invariant and the site, because a state this far wrong renders
 * as something odd rather than as an error. */
#ifndef KG_DEBUG_STATE
#define KG_DEBUG_STATE 0
#endif
#if KG_DEBUG_STATE
void kg_state_check(const char *where);
#define KG_STATE_CHECK(where) kg_state_check(where)
#else
#define KG_STATE_CHECK(where) ((void)0)
#endif

#endif /* KG_BUFHANDLE_H */
