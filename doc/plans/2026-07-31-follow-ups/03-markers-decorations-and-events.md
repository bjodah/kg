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

Status: complete.

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

Status: complete.  Relocation is allocation-free and part of the no-fail
publication tail.  Marker-store growth is covered under a 64 MiB address-space
limit so a capacity regression fails locally instead of exhausting the host.

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

Status: complete for consumers that exist today.

Convert in small commits:

1. isearch's saved point and last match positions;
2. active mark and the mark ring;
3. yank/yank-pop's inserted span in Plan 05;
4. registers and compilation diagnostics as those features land.

There is no yank-pop span, register position, or parsed compilation diagnostic
in the current editor.  The existing single-entry yank records its insertion
start by pushing the active mark, so converting that mark covers its present
position state.  Plan 05 must add a distinct pair of markers when yank-pop adds
the second endpoint; future register and diagnostic work must start marker-
backed rather than introducing interim row/column snapshots.

The active mark and mark ring become marker handles owned by the buffer; remove
parallel `(row,col)` copies in the same commit.  This deliberately changes
behavior: marks now move with edits.  Add PTY cases for insertion/deletion
before and through a mark, `C-x C-x`, `C-u C-SPC`, two windows on one buffer,
buffer switch, kill/reuse, undo, and read-only motion.  Update README/man.

Window point remains window-owned.  A future `save-excursion` uses temporary
markers; do not turn every cursor into a marker without a demonstrated need.

## Phase 3 — Compact decorations

Status: complete.  `src/decor.h`/`decor.c` own the store, the renderer
reads it through a stack-allocated query, and isearch is its first
consumer.  Three details the implementation settled that this text left
open:

- The store is packed rather than hole-punched, unlike the marker store:
  deletion and evaporation compact it in place, so there is no `active`
  bit and no inactive slot to scan past.
- `kg_decor_compact()` serves both jobs the plan describes separately.
  After an ordinary edit it drops evaporating empty spans; after broad
  row adoption, where `kg_marker_detach_all()` has already made every
  endpoint gone, the same pass clears every decoration.
- The renderer keeps at most `KG_ROW_DECOR_MAX` (16) simultaneous spans
  per row and drops the rest rather than allocating.  That is a fidelity
  limit for a future producer, not a limit isearch can reach.

### Data and ownership

Create self-contained `src/decor.h`/`src/decor.c`.  Keep the public handle the
same identity shape as markers: buffer handle, decoration ID, and generation.
The private record contains two marker handles, a compact face enum, an
8-bit priority, and flags for endpoint stickiness and empty-span evaporation.
It contains no strings, runtime values, callbacks, or arbitrary Emacs text
properties.

The buffer owns a growable vector of records.  Creation stages vector capacity
and both endpoint markers before publishing the record; failure deletes any
staged marker and leaves the store unchanged.  Deletion releases both markers.
ID exhaustion refuses creation, stale handles resolve as gone, and buffer kill
frees the decoration store before the buffer identity is invalidated.  Broad
row adoption drops every decoration along with its endpoint markers.

### Ordering and edit integration

Keep live records sorted by resolved start position, then end position,
priority, and ID.  Marker relocation is monotonic, so an edit preserves that
order apart from deterministic ties; new records use binary insertion.  After
the edit transaction relocates markers, compact evaporating empty spans in
place.  This publication-tail pass must allocate nothing and cannot make a
successful text edit fail.

State endpoint gravity explicitly at creation.  A match highlight normally
uses a left-sticky start and right-sticky end; an insertion at either boundary
therefore remains outside the old match unless a consumer requests another
policy.  Normalize reversed endpoints at creation, and treat a gone endpoint
as a gone decoration rather than guessing a range.

### Renderer contract

Expose a read-only iterator/query taking a buffer and visible flat-byte range.
It returns only intersecting spans in sorted order.  The renderer converts each
intersection to row-local chars/render coordinates at the drawing seam,
combines overlapping faces by priority and stable ID, and clips to the current
window.  Querying and drawing must not allocate, mutate stores, call Fe, or run
callbacks.  Add counters for records examined and visible spans returned so
the sorted-vector cost is measured before an interval tree is considered.

### Consumer migration

Migrate isearch first.  One temporary decoration owns the current match;
changing the query or accepting/cancelling the search deletes it.  Remove the
generation-checked `struct hl_snapshot` and all direct match writes to
`row->hl`; the older plans' `RESTORE_HL` macro is already gone and must not be
described as current code.  Query-replace handoff may retain or delete the
decoration only through an explicit ownership transfer, never a copied handle
whose owner also deletes it.

Parsed compilation diagnostics are the second consumer when Plan 05 adds their
data model.  Trailing-whitespace and package-defined decoration producers wait
for Plan 06's hook path; they use this C store and do not receive a renderer
callback escape hatch.

### Proof

Native tests cover create/delete/stale handles, both endpoint gravities,
overlaps and priority ties, evaporation, allocation rollback, ID exhaustion,
broad adoption, buffer kill/reuse, and an edit sequence checked against a flat
reference model.  Renderer tests cover clipping at both viewport edges,
multiple overlapping faces, empty rows, tabs, UTF-8 and malformed bytes.
PTY cases cover isearch accept/cancel, query change, query-replace handoff after
a row join and split, edit between highlight and clear, buffer switch/kill, and
two windows displaying different slices of one decorated buffer.

## Phase 4 — One bounded typed event queue

Status: complete.  `src/event.h`/`event.c` is the queue; its producers are
wired at every publication point named below.  What the implementation
settled:

- The ring is 64 entries (`KG_EVENT_RING_MAX`) of which 4
  (`KG_EVENT_LIFECYCLE_RESERVE`) are reachable only through a
  reservation, so a text event can never crowd out a kill or a save.
- Two lifecycle publications cannot refuse, because their transition has
  already committed by the time the reservation is taken: `killed` (after
  cleanup) and `after-save` (after the write returned).  Losing those
  reservations drops the event, never the operation.  Every other
  lifecycle producer reserves first and refuses with its old state
  intact.
- `editor_set_syntax()` is the mode-changed transition.
  `editor_select_syntax_highlight()`, which runs at open, is deliberately
  not instrumented: buffer-open already reports that.
- Coalescing is narrower than the section below permits.  Only two
  consecutive exact changes to the same buffer, under the same command
  token and adjacent generations, whose second pre-edit span is contained
  in the first's post-edit span, merge — the growing-insertion-point
  shape.  A leftward-growing or disjoint pair stays two events, because
  composing the old-length side exactly would mean relocating a boundary
  backwards through the earlier edit.

### Envelope and storage

Create self-contained `src/event.h`/`src/event.c`.  Define one tagged envelope
whose payload union contains only fixed-size values.  Initial payloads:

- buffer changed: handle, begin, old/new lengths, resulting generation;
- broad buffer replacement/adoption;
- buffer opened, about-to-be-killed, killed;
- view attached/detached;
- before/after save and mode changed;
- process output/exit later through the same envelope.

Events own no borrowed row pointers, filenames, row text, decoration pointers,
or runtime values.  They carry handles/IDs, byte lengths, generations, flags,
and bounded copied names only where a consumer demonstrably needs the name.
The queue is a fixed-capacity ring owned by editor state.  A fixed
`MAX_BUFFERS` overflow table records at most one pending broad-change summary
per live buffer, so pressure never asks the allocator for more memory.

Give lifecycle events reserved capacity.  A producer must reserve its
non-droppable event before publishing the lifecycle transition; if no reserved
slot exists, the operation is refused while its old state is intact.  Text
edits do not need such back-pressure: an exact change that cannot enter the
ring merges into that buffer's broad summary.  Thus allocation or queue
pressure can reduce precision, but cannot silently publish a kill, attach, or
save transition that no subscriber can observe.

### Producer rules

Queue a text event only after successful byte and marker/decor publication.
Its payload is the buffer handle, changed start, old span length, new span
length, and resulting content generation.  A refused, failed, read-only, or
byte-identical edit queues nothing.  Reload and broad row adoption queue one
broad-replacement event containing the old/new total lengths and resulting
generation.

Buffer open queues after the slot has a resolvable identity.  About-to-kill
queues while the handle still resolves; killed queues after cleanup and keeps
the dead handle only as identity evidence.  View attach/detach payloads carry
both stable view and buffer handles once Plan 04 supplies them.  Before-save is
reserved before I/O; after-save says success/failure and never pretends a
failed write changed buffer state.

Start with no edit coalescing beyond overflow collapse.  Add ordinary
coalescing only for consecutive text events from the same buffer, same
top-level command token, and adjacent generations.  The merge computes one
conservative affected envelope in the post-edit coordinate system; if that
calculation is not exact and overflow-safe, keep two events or promote them to
a broad change.  Never coalesce across lifecycle, save, mode, process, or
callback-drain boundaries.

### Pressure and ordering

Drain ring entries in publication order.  When an overflow summary is needed,
remember the sequence number of the first exact event it replaces; inject the
broad event at that ordering point rather than appending it after later
lifecycle events.  Repeated changes to the same buffer update its new length
and generation while preserving the first old length.  A kill retains its
preceding broad change, then removes any later impossible change summary for
that dead identity.  Slot reuse creates a different handle and therefore a
different summary entry.

### Proof

Native tests cover every payload constructor, refused/no-op edits, exact
ordering, conservative coalescing, arithmetic boundaries, ring wraparound,
overflow collapse for one and several buffers, lifecycle reservation failure,
kill before drain, and slot reuse.  A model test generates publications into a
small-capacity queue and verifies that each successful mutation is represented
by either its exact event or a broad event spanning it.  Build and run the same
tests with `WITH_LISP=0`; the module contains no Fe types or conditionals.

## Phase 5 — C safe-point delivery

Status: complete.  Three safe points in `src/main.c`'s loop: after
`autorevert_poll()`, before `editor_refresh_screen()`, and after
`editor_process_keypress()` returns.  Prompt deferral is a nesting depth
kept in `event.c` and raised by the four functions that actually read a
prompt, so `kg_event_drain_safe()` is a no-op until the outermost one
leaves.  The drain captures `next_seq - 1` as its boundary and
`queue_pop_bounded()` refuses anything above it, for ring entries and
overflow summaries alike.

Two limits recorded rather than fixed:

- An overflow summary that already existed when a drain began, and is
  *extended* by a callback during that drain, is not separately gated by
  the boundary; only a summary first created during the drain is.  It
  needs the ring to be under overflow pressure and a callback to edit
  that same buffer in the same drain.
- The debug-build assertion covers an active edit transaction, an active
  renderer, an open prompt and a recursive drain, but not a signal
  handler.  `handle_sig_winch()` only sets `pending_resize` for the main
  loop to read; it reaches no buffer, render or event code, so the
  condition cannot arise and instrumenting a real POSIX signal handler
  for it was not worth the cost.

Wiring the drain also closed a regression the producer slices opened: with
nothing draining, 60 edits plus a few lifecycle transitions exhausted the
reserve and the editor began refusing saves.
`test/pty/event-drain-save-after-sustained-editing.yaml` and
`test_drain_relieves_the_reserve_a_sustained_session_would_exhaust` pin it
from both sides.

### Subscriber registry

Define a small fixed-capacity C subscriber registry in `event.c`.  Registering
returns a generation-checked token; a record contains a C function pointer,
opaque C context pointer, event-kind mask, registration sequence, and active
bit.  Dispatch order is registration order.  Exhaustion refuses registration,
unregister is idempotent for a live token, and slot reuse cannot let an old
token remove a new subscriber.

Registry mutation during dispatch has explicit semantics: unregistering a
subscriber before its turn skips it, including self-unregister; a subscriber
registered during a drain is not eligible until the next drain.  Snapshot
tokens, not callback pointers, at the drain boundary and re-resolve each token
before calling it.

### Named safe points

Add `kg_event_drain_safe()` calls only at named top-level boundaries in the
main/input loop: after one command and its edit group has completed, after a
completed non-command lifecycle action, and before the next screen refresh.
Assert in debug builds that no edit transaction, renderer, signal handler,
minibuffer read, confirmation prompt, or recursive drain is active.  A prompt
sets a deferral flag; leaving the outermost prompt restores eligibility rather
than draining from prompt cleanup.

At the start of a drain, capture its last sequence number.  Deliver only events
at or below that boundary.  Events queued by callbacks remain for the next
top-level drain even when capacity remains, which prevents same-drain
recursion and gives every callback a stable batch.  Overflow summaries obey
the same generation boundary.

For each delivery:

- no edit is partial;
- no renderer or signal handler is active;
- prompt state explicitly allows or defers delivery;
- each payload is re-resolved from its handle;
- work queued by a callback waits for the next drain, preventing recursion.

Resolution policy is event-specific.  Change/open/view events whose live
handle no longer resolves are still delivered with a `gone` resolution status
so observers can discard cached state.  About-to-kill must resolve; killed is
expected not to.  No dispatcher turns a dead handle into the current occupant
of the same slot.

### Callback containment and ownership

C callbacks return a small status (`continue`, `unsubscribe`, `error`) rather
than long-jumping through the loop.  One callback's error is recorded and does
not suppress later subscribers.  Plan 06's runtime adapter adds its own step
budget, roots, cancellation and Fe error conversion inside one C subscriber;
none of those types enter the core queue.

Core ownership cleanup does not depend on callbacks: markers/decorations die
directly with their buffer, and process descriptors with their process owner.
Callbacks observe lifecycle; they do not make it correct.

Plan 04 adds window/buffer lifecycle producers here.  Plan 06 adds a runtime
subscriber using the same queue, step budgets, root ownership, and error
isolation.  Do not add a second Lisp hook queue.

### Proof

Unit tests use callbacks that record delivery order, unregister themselves or
another callback, register replacements, queue edits and lifecycle work, and
return errors.  They prove deterministic order, next-drain deferral, prompt
deferral, no recursive drain, stale subscriber tokens, dead buffer handles,
and preservation of later callbacks after one error.  Integration tests place
assertions around command dispatch, minibuffer/confirmation reads, rendering,
buffer kill, view changes, save, and process completion.  A test-only drain
depth counter must never exceed one.

## Completion gate

Met 2026-08-01.  27 native suites and 330 PTY cases pass with zero skips
and zero XPASS in both `WITH_LISP` configurations, and all twelve
`.ci/ci-NN-*.sh` stages pass.  `SCC_COMPLEXITY_MAX` is 4450 by the
maintainer decision recorded in this directory's README; measured usage is
4431, and the loan's named repayment sources are still outstanding —
`src/search.c` gave back 7 (246 → 239) when the snapshot machinery went,
`src/cmd.c` and `src/localvars.c` have not been touched.

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
