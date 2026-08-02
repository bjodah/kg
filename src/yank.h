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
 * Only the newest entry is reachable today: C-y always yanks entries[0].
 * The ring holds more so a later slice can add M-y (yank-pop) to walk
 * older entries without another storage change; that slice owns the
 * command-transient state (which entry M-y last inserted, where, how
 * long) -- this module stores bytes only, nothing about a yank in
 * progress.
 */

#ifndef KG_YANK_H
#define KG_YANK_H

#include <stddef.h>

#define KG_KILL_RING_MAX_ENTRIES 16
#define KG_KILL_RING_MAX_BYTES ((size_t)8 * 1024 * 1024)

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

/* The newest entry's bytes, or NULL if the ring is empty.  `len` bytes
 * are followed by one defensive NUL for callers that read it as text;
 * that byte is not counted in kill_ring_get_len() and is not present
 * inside the span for a caller that must honor an embedded NUL. */
char *kill_ring_get(void);

/* The newest entry's length, or 0 if the ring is empty. */
size_t kill_ring_get_len(void);

/* Test-only allocation-failure seam for this module's own entry storage
 * (kill_ring_set()/kill_ring_append()'s malloc of one entry's bytes) --
 * distinct from kg_edit_fail_alloc_after() in edit.h and
 * kg_decor_fail_alloc_after() in decor.h, one seam per allocator.  Lets
 * the next `n` such allocations succeed and fails the one after that,
 * then disarms itself; a negative `n` disarms it immediately, which is
 * how the editor always runs.  Nothing outside test/ calls this. */
void kg_yank_fail_alloc_after(int n);

#endif /* KG_YANK_H */
