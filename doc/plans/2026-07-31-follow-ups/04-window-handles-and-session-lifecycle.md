# Plan 04 — Window handles and session lifecycle

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
