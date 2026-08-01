# Plan 04 — Window handles and session lifecycle

## Status (2026-08-02, after Plan 03 landed)

Phases 0-3 are complete; Phase 4 remains deferred, still waiting on Plan
05's kill ring, and nothing outside it depends on the rename.

Phase 3 arrived in two halves.  Plan 03's producer wiring published
buffer opened/about-to-kill/killed and view attached/detached from
`buf_claim_slot()`, `buf_kill()`, `buf_attach_view()` and the canonical
detach path, on the one bounded queue with safe-point delivery — no
parallel hook list exists.  This plan then supplied what that wiring had
to fake:

- **Windows have their own identity.**  `struct kg_window_handle` sits
  beside `kg_buffer_handle` in `src/bufhandle.h`, same 64-bit
  never-repeating id, same refusal to reuse at `UINT64_MAX`.
  `struct kg_event_view` carries it instead of the raw `winlist[]` index
  it was holding as a placeholder.  A split claims a *new* identity for
  the copied slot — the one place the buffer-handle rule is deliberately
  inverted, since a split makes a second view, not a second name for one.
- **Both handle families share one identity check.**  `kg_slot_table`
  and `kg_slot_resolve()` in `bufhandle.h` address a slot table by
  layout, so the second family cost call sites rather than a second copy
  of `buf_handle()`/`buf_resolve()`.  That sharing is most of what paid
  for the family: `bufmgr.c` fell 429 → 417.  `kg_slot_resolve()` hands
  back the *record*, not a verdict the caller re-indexes with, because
  splitting those made clang-analyzer report an out-of-bounds access it
  could no longer see was guarded.
- **The seventh invariant is armed.**  It had nothing to check until
  markers and decorations existed.  `kg_state_check()` now walks every
  buffer's mark, mark ring and decoration endpoints, asserting each
  handle resolves *and* names that buffer — a corrupted `.buffer` field
  that still resolves is the failure resolving alone would miss.

One real defect fell out of writing Phase 3's ordering tests: both split
functions copy the window struct wholesale rather than calling
`buf_attach_view()`, deliberately, so the new view keeps the split's
point instead of the buffer's remembered one — but that made the new
view's attach invisible to the queue, and no `KG_EVENT_VIEW_ATTACHED`
ever fired for it.

### Status (2026-07-31, after the handle campaign)

Phases 0, 1 and 2 are complete.  `editor_window.bufidx` is gone:
`struct editor_window` holds a `kg_buffer_handle`, `win_buffer()` /
`win_buffer_slot()` / `win_shows_buffer()` are the only readers, and
`win_buffer_slot()` answers -1 rather than an index for a handle that has
died.  `win_check_handles()` runs at the top of the main loop, before the
repaint.  `KG_DEBUG_STATE` arms six of the seven invariants and is on in
`.ci/ci-04`.  scc went 4116 → 4123 against the 4127 cap: the flag day
itself was −9 (six duplicated window scans collapsed onto one predicate;
two functions stopped distinguishing `wcur()` from the window already in
hand), and the invariant checker spent +16 of that back.

Deliberately not done *at that time*; the first and third are now done,
and the second still holds:

- **The seventh invariant has nothing to check.**  Buffer-owned marker
  and decoration handles arrive with Plan 03.
- **No new perf counter.**  `KG_PERF_HANDLE_STALE` now counts only a
  handle that once named a buffer and no longer does — a zeroed view is
  asked about on every window scan and no longer inflates it — so a
  nonzero value already *is* the invariant question, and a second
  "views repaired" counter would answer it twice.
- **Phase 3 is blocked** on Plan 03's event queue, and a parallel
  callback registry is not a substitute.
- **Phase 4 is deferred** by the program's ordering.

Buffer identity is 64-bit.  The claim that the counter "never repeats" is
the whole reason it exists, and 32 bits does not get to make it: 4.29
billion claims is a number, and a wrapped id would hand a live buffer the
identity a long-dead one's handle still holds.  A claim that reaches
`UINT64_MAX` refuses to reuse, leaving the slot with id 0, which
`buf_resolve()` never matches.

## Decision — auto-revert stays restricted to `buf_current`

Taken 2026-07-31 after characterizing the reload
(`test_winmgr.c`: poll reverts only the current buffer; a deferred revert
clamps a point past the new EOF; a revert drops the region and the undo
chain; a revert clamps every window on the buffer).  The poll keeps
reverting only `buf_current`; other changed buffers stay flagged and
reload at the next `buf_select()`.  Reasons, in order of weight:

1. **A reload destroys state the user cannot see it destroy.**
   `buf_reload_from_disk()` clears `mark_set`, `mark_highlight`,
   `shift_select` and `rect_mode`, and frees the undo chain.  For the
   current buffer the user is watching the buffer change.  For a
   background buffer, a later `C-x b` finds the region gone and `C-_`
   with nothing to undo, and no event said so.
2. **Nothing is being fixed.**  A flagged buffer already reloads at the
   next `buf_select()`, which is the first moment its content is
   observable.  Widening only moves *when*, to the moment with no user
   present.
3. **The reload chain is written against `bcur()`.**
   `commit_load_result()` (`fileio.c`), `undo_free()`/`undo_init()`/
   `undo_mark_clean()` (`undo.c`) and `buf_apply_local_settings()` all
   mean the current buffer.  Widening means either threading a buffer
   through three more modules or rebinding `buf_current` around the
   reload — the copy protocol this codebase spent commits deleting.
4. **The prompt is unaffected either way.**
   `confirm_save_over_accepted()` only fires for a dirty buffer, and a
   revert never runs against one.

The multi-window half of the plan's premise *was* a real defect and is
fixed independently: `silent_revert_current()` now clamps every window on
the reverted buffer, not just the selected one.  That is also the
prerequisite a future widening would need, so revisiting this decision is
a policy change and no longer a plumbing project.

## Outcome

An active window never names a buffer by a reusable slot alone.  Buffer/view
lifecycle invariants are checked at top-level commits and published through the
single event queue.  Session state is grouped only after keymap and kill-ring
state have reached their final shape.

The handle work is correctness hardening and may start now.  Session nesting is
cleanup, not an architectural prerequisite.

## Phase 0 — Close the current detach asymmetry

`win_delete_others()` deactivates windows without banking their views.  Add a
focused native/PTY case, call the canonical remember/detach path for each
discarded view, and preserve the point seen when that buffer is shown again.
Do not combine this behavior fix with the handle flag day.

Also decide auto-revert's restriction in its own commit.  Today the poll
reverts only `buf_current` — `silent_revert_current()` can restore only the
active window's view — while other changed buffers merely mark `disk_changed`
and revert on the next `buf_select()`.  Once every window is enumerable, a
buffer shown in inactive windows can be reverted with each view clamped to
the new bounds.  Characterize multiple windows, point past new EOF, active
mark, and changed-on-disk prompt behavior before widening it.

## Phase 1 — Window buffer handles

Make the buffer handle type available before `struct editor_window` through a
self-contained ownership header or a deliberate declaration move.  Replace
`editor_window.bufidx` with a generation-checked handle.  A zero handle denotes
a detached/inactive view.

Add canonical helpers:

- resolve a window's buffer;
- compare a window to a buffer handle;
- attach, remember, and detach a view;
- obtain a slot only after successful resolution.

Release builds must never use a failed resolution as an array index.  At a
safe top-level point, stale active handles trigger a conservative fallback or
window detachment and a clear diagnostic.  Debug builds assert that normal
control flow never produces them.

Migrate in small cohorts: initialization/attach, split/cycle/delete, buffer
kill/reassignment, display, cursor/position helpers, tests and fuzz stubs.
Copying a window during split copies the stable handle intentionally.

Buffer-kill ordering:

1. retain the dying handle while it still resolves;
2. clean up buffer-owned markers/decorations directly;
3. detach/reassign every matching view and remember appropriate points;
4. publish about-to-kill/detach events when the queue exists;
5. invalidate the buffer generation and later publish killed;
6. handle the last-buffer exit without a transient active stale window.

Review the claim that 32-bit IDs "never repeat".  Prefer 64-bit monotonic IDs
with checked exhaustion, or explicitly refuse reuse at wrap; do not let a
wrapped ID make a stale handle live.

## Phase 2 — State invariant checker and accounting

Add a dedicated debug-state build flag and arm it in an existing sanitizer/
debug CI lane.  At lifecycle commits, check:

- active counts agree with `buf_count`/`win_count`;
- live buffer identities are unique;
- every active window handle resolves;
- selected window resolves to `buf_current`;
- row count/capacity/indices are coherent;
- live row storage has one owner;
- buffer-owned marker/decoration handles resolve to that buffer.

Keep the existing stale-handle perf counter and add only counters that answer a
real invariant question.  Extend `test/test_winmgr.c` — today an example-based
characterization suite, not a model — with model-style checks for split
copies, buffer shown twice, kill/reuse, last buffer, slot exhaustion, and
injected stale display input.

## Phase 3 — Lifecycle events, not a second callback registry

After Plan 03's event queue exists, publish buffer-opened/about-to-kill/killed
and view-attached/detached at `buf_claim_slot()`, `buf_kill()`,
`buf_attach_view()`, and the canonical detach/remember path.  Use the same
bounded ordering, safe-point drain, and handle re-resolution as edit events.

Do not make buffer correctness depend on observers.  Marker cleanup, view
reassignment, process ownership release, and generation bumps happen in their
owners even if the queue is full or no subscriber exists.

Tests pin ordering for open/attach, split, detach, kill shown in two windows,
kill before drain, and slot reuse.  Queue overflow must retain lifecycle truth.

## Phase 4 — Session nesting, deliberately last

Start only after:

- Plan 01 has replaced `cx_prefix`, `cc_prefix`, `rect_prefix`, numeric-prefix
  fields, and raw last-key state with final keymap/command state; and
- Plan 05 has finalized the bounded kill ring and decided that rectangle kills
  remain separate.

First extract self-contained `src/session.h`.  Then use mechanical commits,
one ownership family at a time:

1. echo/status/minibuffer presentation state;
2. terminal/input state and dimensions;
3. command/key traversal and command-cycle state;
4. finalized kill-ring/session services;
5. remaining process-wide flags only when their ownership is truly session.

The target is a `struct kg_session` with no field duplicated in a buffer or
window.  Keep rectangle storage module-private unless a real shared kill-store
abstraction justifies moving it.  Do not smuggle behavior changes into the
rename; declaration extraction and mechanical moves should be complexity
neutral and may bank header decomposition separately.

If this phase neither reduces `def.h`, clarifies an ownership boundary, nor
funds a ratchet, it may remain deferred.  Nothing in Plans 02, 03, 05, 06, or
07 depends on the rename itself.

## Completion gate

- No active window stores or dereferences a bare buffer slot.
- View deletion/kill/reuse preserves or deliberately clamps point.
- Debug invariants cover all top-level lifecycle commits.
- Lifecycle notifications use Plan 03's queue; no parallel hook list exists.
- Session nesting, if performed, follows final keymap/kill-ring state and
  leaves no duplicated buffer/window fields.
- Native model, PTY window/buffer cases, fuzz invariants, both Lisp builds,
  sanitizer/debug lane, static analysis, and full CI pass.
