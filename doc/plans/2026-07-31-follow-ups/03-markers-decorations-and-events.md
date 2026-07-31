# Plan 03 — Markers, decorations, and typed events

## Outcome

Persistent positions survive edits through stable markers; temporary visual
ranges use compact C-owned decorations; committed mutations and lifecycle
changes enter one bounded typed event queue and run C callbacks only at named
safe points.  Fe delivery is a consumer added by Plan 06, not part of the core
ownership model.

Marker consumer conversion starts only after Plan 02 has routed every
observable live-buffer mutation through a publication seam.

## Phase 0 — Marker position and lifetime contract

Create self-contained `src/marker.h`/`src/marker.c`; do not add module-owned
declarations to `def.h`.

External code holds a marker handle, not a pointer or array index:

```c
struct kg_marker_handle {
	struct kg_buffer_handle buffer;
	uint32_t id;
	uint32_t generation;
};
```

A marker record stores a flat byte position, left/right insertion gravity, and
active state.  The buffer owns a bounded/growable marker store and releases it
on buffer kill.  IDs never silently alias after slot reuse; widen counters or
refuse creation at exhaustion rather than relying on integer wrap.

Specify replacement `[begin,end)` relocation:

- before `begin`: unchanged;
- after or at `end`: shift by the signed byte-length delta;
- inside a deleted range: collapse to the inserted range's start/end according
  to gravity;
- at an empty insertion point: gravity chooses before/after inserted bytes;
- broad buffer adoption/reload: either relocate through an explicit whole-span
  replacement or detach markers according to that operation's documented
  policy; never retain unchecked old offsets.

Resolution returns live/gone/type-error distinctly.  Converting a marker to
row/column or to Lisp's codepoint position happens at the boundary; marker
storage remains byte-positioned.

## Phase 1 — Relocation in the publication commit

Extend the edit staging/commit record with everything marker publication needs.
Marker relocation itself should not allocate: validate position arithmetic
before text publication, then update live records during the no-fail commit.
Broad row adoption invokes the same relocation/invalidation contract.

Native reference-model tests generate arbitrary byte strings, markers with
both gravities, and edit sequences.  After every edit compare all live marker
positions to a flat-string model.  Include:

- BOB/EOF and empty buffers;
- insertion exactly at a marker;
- deletion containing one or both endpoints;
- multiline replacements and final-newline policy;
- UTF-8 and malformed bytes (positions are bytes, never split implicitly);
- buffer kill, slot reuse, marker deletion/reuse, and ID exhaustion seam;
- edit allocation failure: marker set is bit-identical.

Only after these tests and Plan 02's live-gateway completion may consumers move.

## Phase 2 — Convert built-in position consumers

Convert in small commits:

1. isearch's saved point and last match positions;
2. active mark and the mark ring;
3. yank/yank-pop's inserted span in Plan 05;
4. registers and compilation diagnostics as those features land.

The active mark and mark ring become marker handles owned by the buffer; remove
parallel `(row,col)` copies in the same commit.  This deliberately changes
behavior: marks now move with edits.  Add PTY cases for insertion/deletion
before and through a mark, `C-x C-x`, `C-u C-SPC`, two windows on one buffer,
buffer switch, kill/reuse, undo, and read-only motion.  Update README/man.

Window point remains window-owned.  A future `save-excursion` uses temporary
markers; do not turn every cursor into a marker without a demonstrated need.

## Phase 3 — Compact decorations

Create `src/decor.h`/`src/decor.c`.  A decoration is two markers plus a small
face enum, priority, optional bounded ID, and stickiness/evaporation flags.  It
is not arbitrary Emacs text properties.

Use a sorted vector per buffer first.  The renderer queries only visible spans
and never allocates or calls runtime code.  Edits relocate endpoints through
markers and remove evaporating empty spans.  Buffer kill owns final cleanup.

First consumer: isearch match highlighting.  Delete the interim
generation-checked `struct hl_snapshot` in `src/search.c`; the older plans'
`RESTORE_HL` macro is already gone and must not be described as current code.
Prove cancellation, query-replace handoff after a row join/split, edit between
highlight and clear, buffer switch/kill, tabs, UTF-8, and split-window drawing.

Second consumer: parsed compilation diagnostics.  Trailing-whitespace or other
package decorations wait for Plan 06's hook path.  Measure visible-query cost
before considering an interval tree.

## Phase 4 — One bounded typed event queue

Create self-contained `src/event.h`/`src/event.c`.  Initial payloads:

- buffer changed: handle, begin, old/new lengths, resulting generation;
- broad buffer replacement/adoption;
- buffer opened, about-to-be-killed, killed;
- view attached/detached;
- before/after save and mode changed;
- process output/exit later through the same envelope.

Events own no borrowed row pointers, filenames, or runtime values.  They carry
handles/IDs and bounded copied data.  Adjacent compatible edits from one
top-level command may coalesce only under a tested rule.  On queue pressure,
collapse fine-grained changes into one broad-change event per buffer; never
drop ownership/lifecycle transitions or grow without bound.

Queue after successful publication.  Refused/no-op/failed edits enqueue
nothing.  Reload/adoption emits one broad event.  Every published live change
therefore has one exact or one broad notification.

Tests cover ordering, coalescing, overflow collapse, buffer kill before drain,
handle reuse, callbacks queuing new work, and `WITH_LISP=0`.

## Phase 5 — C safe-point delivery

Define a small C subscriber registry with deterministic order and bounded
subscriber count.  Drain only at top-level safe points in the main/input loop:

- no edit is partial;
- no renderer or signal handler is active;
- prompt state explicitly allows or defers delivery;
- each payload is re-resolved from its handle;
- work queued by a callback waits for the next drain, preventing recursion.

Core ownership cleanup does not depend on callbacks: markers/decorations die
directly with their buffer, and process descriptors with their process owner.
Callbacks observe lifecycle; they do not make it correct.

Plan 04 adds window/buffer lifecycle producers here.  Plan 06 adds a runtime
subscriber using the same queue, step budgets, root ownership, and error
isolation.  Do not add a second Lisp hook queue.

## Completion gate

- The randomized marker model agrees after every generated edit.
- No converted consumer retains a parallel row/column snapshot.
- Isearch highlight is a decoration; private `row->hl` snapshots are gone.
- Every observable mutation/lifecycle publication produces an exact or broad
  bounded event.
- Callbacks run only from asserted safe points and cannot recurse in the same
  drain.
- Core builds and tests with Lisp disabled and contains no Fe types.
- Both configurations, sanitizers, fuzz seeds, marker/event native tests, PTY
  behavior, and the full runner pass without a ratchet increase.
