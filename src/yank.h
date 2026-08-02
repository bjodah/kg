/* yank.h — bounded kill ring storage
 *
 * The kill ring is a small, fixed-capacity stack of killed/copied spans:
 * at most KG_KILL_RING_MAX_ENTRIES entries and KG_KILL_RING_MAX_BYTES
 * bytes across all of them combined.  entries[0] is newest; a push
 * (kill_ring_set()) evicts the oldest entries, in order, until both caps
 * hold, and rejects outright (leaving the ring untouched) an entry too
 * big to ever fit even alone.  Every entry carries its length alongside
 * its bytes -- an embedded NUL is ordinary data, never a terminator.
 *
 * C-y always yanks entries[0]; M-y (yank-pop) walks older entries from
 * there.  yank-pop's own transient state -- which entry it last
 * inserted, where, how long, and how many copies -- is a file-static
 * struct in yank.c, not a member here: this module stores bytes only,
 * nothing about a yank in progress.  `generation` is the one piece of
 * that story the ring itself carries, bumped by every call that changes
 * entries[] (push, in-place growth, or a free), so a stale yank-pop
 * record can tell a mutation happened even in the hypothetical case
 * where something other than the coalescing class (see cmdstate.h's
 * kill_coalesce_class, which already gates every ordinary path) let one
 * through.
 */

#ifndef KG_YANK_H
#define KG_YANK_H

#include <stddef.h>

#include <stdint.h>

#define KG_KILL_RING_MAX_ENTRIES 16
#define KG_KILL_RING_MAX_BYTES ((size_t)8 * 1024 * 1024)

/* The largest span a single insertion -- a repeated yank (C-u N C-y) or
 * a yank-pop replaying that same N against a different entry -- may
 * produce.  Independent of KG_KILL_RING_MAX_BYTES: an entry is capped
 * per copy, this caps N copies of it laid end to end. */
#define KG_YANK_BATCH_MAX ((size_t)8 * 1024 * 1024)

/* One killed/copied span.  `len` is authoritative; `text` is arbitrary
 * bytes that may contain an embedded NUL. */
struct kill_ring_entry {
	char *text;
	size_t len;
};

/* entries[0] is newest, entries[count-1] oldest.  Slots at and past
 * `count` are always zeroed ({NULL, 0}), which is what lets
 * kill_ring_get()/kill_ring_get_len() read entries[0] unconditionally.
 * `total_bytes` is the sum of every live entry's `len`, kept in
 * lock-step so a push can check the byte cap without walking the ring. */
struct kill_ring {
	struct kill_ring_entry entries[KG_KILL_RING_MAX_ENTRIES];
	int count;
	size_t total_bytes;
	/* Bumped by every push or in-place growth (kill_ring_set(),
	 * kill_ring_append()/kill_ring_prepend() when they actually change
	 * an entry) and by kill_ring_free().  Not bumped by a rejected
	 * oversize entry or an OOM that left the ring untouched, since
	 * nothing about entries[] changed either time. */
	uint32_t generation;
};

extern struct kill_ring killring;

void kill_ring_init(void);
void kill_ring_free(void);

/* Start a new newest entry (Emacs' kill-new): `text`/`len` become
 * entries[0], ahead of whatever was there.  An entry bigger than
 * KG_KILL_RING_MAX_BYTES is rejected outright, unchanged ring.
 * Otherwise the oldest entries are evicted, in order, until the
 * entry-count and total-byte caps both hold; an allocation failure also
 * leaves the ring bit-identical to before the call.  len==0 is a no-op. */
void kill_ring_set(const char *text, size_t len);

/* Grow the newest entry in place (Emacs' kill-append), or start one if
 * the ring is empty.  Same oversize-rejection/eviction/OOM-preserves-old
 * rules as kill_ring_set(), applied to the combined entry.  len==0 is a
 * no-op. */
void kill_ring_append(const char *text, size_t len);

/* Grow the newest entry by adding `text`/`len` *before* its existing
 * bytes, or start one if the ring is empty.  This is what a backward
 * kill (M-Backspace, C-w with point behind the mark) uses: two
 * consecutive backward kills each prepend the text they just removed,
 * so the entry reads in buffer order rather than reversed.  Same
 * oversize-rejection/eviction/OOM-preserves-old rules as
 * kill_ring_set(), applied to the combined entry.  len==0 is a no-op. */
void kill_ring_prepend(const char *text, size_t len);

/* Record a forward kill's bytes: text that disappeared ahead of point,
 * point unchanged (C-k, M-d, M-z, C-w with point ahead of the mark).
 * Appends to the newest entry when the previous top-level command's
 * coalescing class was KILL_COALESCE_KILL (see cmdstate.h) -- any kill
 * producer, not just this one -- otherwise starts a fresh entry.  Marks
 * this command KILL_COALESCE_KILL so whatever kill runs next may
 * coalesce with it. */
void kill_ring_kill_forward(const char *text, size_t len);

/* Record a backward kill's bytes: text that disappeared behind point,
 * point moved left (M-Backspace, C-w with point behind the mark).
 * Prepends instead of appends; same coalescing rule as
 * kill_ring_kill_forward(). */
void kill_ring_kill_backward(const char *text, size_t len);

/* Record a copy (M-w / kill-ring-save): always starts a fresh entry and
 * marks this command KILL_COALESCE_COPY, which is not itself
 * coalescing-eligible -- neither a repeated copy nor a kill that follows
 * one grows an existing entry. */
void kill_ring_copy(const char *text, size_t len);

/* The newest entry's bytes, or NULL if the ring is empty.  `len` bytes
 * are followed by one defensive NUL for callers that read it as text;
 * that byte is not counted in kill_ring_get_len() and is not present
 * inside the span for a caller that must honor an embedded NUL. */
char *kill_ring_get(void);

/* The newest entry's length, or 0 if the ring is empty. */
size_t kill_ring_get_len(void);

/* `n` copies of entries[index]'s bytes, concatenated into one fresh
 * allocation, with `*out_len` set to its length.  NULL, `*out_len`
 * untouched, for n <= 0, an out-of-range index, a result over
 * KG_YANK_BATCH_MAX, or an allocation failure.  Shared by a repeated
 * yank (C-u N C-y, always index 0) and yank-pop replacing the same span
 * with N copies of a different entry -- which is why the copy count
 * travels with the span (see kill_ring_note_yank()) rather than being
 * re-read from whatever prefix argument M-y itself was given. */
char *kill_ring_entry_repeated(int index, int n, size_t *out_len);

/* Record that a yank just inserted `inserted_len` bytes at flat position
 * `start`, `repeat_count` copies of the ring's newest entry (1 for a
 * plain C-y, N for C-u N C-y) -- what M-y needs to replace that span
 * with the next-older entry, repeated the same number of times.  Starts
 * a fresh marker at `start` (deleting whatever marker a previous,
 * unfinished yank-pop sequence left behind, so the record never
 * accumulates more than one), and marks this command KILL_COALESCE_YANK,
 * which is the only thing that makes M-y eligible on the next keystroke.
 * A marker allocation failure leaves this yank a plain insertion: no
 * eligibility, same as if nothing had been noted.  Called by
 * editor_yank() and key_yank_repeated(), the two producers of an
 * eligible span; yank-pop updates its own record in place instead of
 * calling this again. */
void kill_ring_note_yank(size_t start, size_t inserted_len, int repeat_count);

/* M-y: replace the span the immediately preceding yank or yank-pop
 * inserted with the next-older entry in the kill ring (wrapping past the
 * oldest back to the newest), repeated the same number of times the
 * originating yank was.  Eligible only when cmd_last_kill_class() reads
 * KILL_COALESCE_YANK and the record kill_ring_note_yank() left resolves
 * in the current buffer -- a different buffer, a stale marker, or an
 * intervening command of any other kind (which already clears the
 * coalescing class) all refuse without changing the buffer.  One undo
 * record per call, merged with the one beneath it on the stack so a
 * single editor_undo() undoes however many yank-pops chained, back to
 * the state before the first yank (see undo_merge_at_top() in def.h). */
void editor_yank_pop(void);

/* Test-only allocation-failure seam for this module's own entry storage
 * (kill_ring_set()/kill_ring_append()'s malloc of one entry's bytes) --
 * distinct from kg_edit_fail_alloc_after() in edit.h and
 * kg_decor_fail_alloc_after() in decor.h, one seam per allocator.  Lets
 * the next `n` such allocations succeed and fails the one after that,
 * then disarms itself; a negative `n` disarms it immediately, which is
 * how the editor always runs.  Nothing outside test/ calls this. */
void kg_yank_fail_alloc_after(int n);

#endif /* KG_YANK_H */
