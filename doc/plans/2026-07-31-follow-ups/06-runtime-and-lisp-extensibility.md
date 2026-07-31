# Plan 06 — Runtime and Lisp extensibility

## Outcome

Fe code calls editor services through a Fe-free adapter, under explicit budget,
interrupt, cleanup, object-lifetime, and safe-point rules.  Buffer/marker/
process objects are generation checked.  Hooks and process filters consume the
same bounded event queue as C.  `WITH_LISP=0` remains the complete built-in
editor.

## Verified blockers

- `src/lisp.c` is about 2,200 lines and still uses a source-string trampoline
  because Fe has `FeCall()` but no `FeCallWithOptions()`.
- Fe unwind/cleanup is design-only (`fe/doc/unwind-design.md`); therefore
  `save-excursion`, `with-current-buffer`, and true atomic change groups are not
  yet safe to promise.
- Buffer handles exist.  Marker handles wait for Plan 03; process handles wait
  for a process table.
- `bcur()` is the active window's buffer.  Hidden Lisp buffer selection cannot
  be reintroduced as a temporary global buffer swap.
- Process spawn/reap is shared, but spawning is shell-command-only and there is
  no bounded multi-process table.
- Hook/event delivery does not exist until Plan 03.
- Keymap, mode, typed-variable, minibuffer-session, and runtime-descriptor
  prerequisites are separate Plan 01/later registry milestones; do not refer
  to them vaguely as "the registries."

## Phase 0 — Decompose the adapter without behavior change

Keep one public, Fe-free `src/lisp.h`.  Split implementation into real
`lisp_*.c` adapter modules with private self-contained headers: runtime/frame
lifecycle, conversion/string helpers, editor natives, command/root registry,
and load/package support.  Update the contributor rule and add a grep-able
check allowing `fe.h` only in these adapter implementation files.

Centralize frame enter/leave, diagnostic labels, scratch cleanup, argument
validation, root ownership, and error translation.  Move
`cmd_eval_last_sexp`'s declaration to `cmd.h`, matching its implementation.
This phase must be behavior- and complexity-neutral, keep both Lisp builds
linkable, and reduce per-file complexity before new runtime surface lands.

## Phase 1 — Direct budgeted Fe calls

In the Fe submodule, add:

```c
FeCallWithOptions(ctx, callable, args, count, options)
```

It must share evaluation step accounting, interrupt polling, GC restoration,
and error behavior with the existing option-aware evaluation entry points.
Test success, wrong arity/error, deterministic step exhaustion, interrupt,
nested ambient budget, and GC checkpoint restoration.  Document the kg-side
divergence in `doc/fe-upstream.md`, commit Fe first, then move the kg pin.

In kg, call the rooted command directly and delete `pending_command`,
`native_run_pending`, `internal--run-pending-command`, and the constant source
trampoline.  Retain the current top-level-only recursion policy initially:
callbacks produced during a callback queue for the next safe-point drain.
One callback failure is isolated and does not prevent later queued callbacks.

## Phase 2 — Explicit runtime execution context and buffer objects

Represent exposed objects with kind, ID, and generation; never raw pointers,
slot indices, or PIDs.  If Fe custom per-context types are still unavailable,
use an opaque adapter-owned registry/representation with strict type checks and
stable printing, then migrate only when the Fe type facility lands.

Define a runtime execution context separate from active-window globals:

```text
selected buffer handle + runtime point marker + optional mark/match state
```

Entry from an interactive command initializes it from the active window.
Hidden-buffer operations resolve the explicit handle and do not select a
window, move displayed point, or change `buf_current`.  Define exactly when a
successful interactive call synchronizes point back to its window.

Add buffer APIs incrementally: current-buffer, name/list/get/create,
buffer-live-p, set explicit runtime buffer, and kill buffer.  Defer
`with-current-buffer` until unwind exists.  Test slot reuse, kill during queued
work, hidden edits/undo, two windows, stale handles, and `WITH_LISP=0`.

## Phase 3 — Editing, search, and marker primitives

All mutations use Plan 02's gateway and preserve read-only policy.  Add one
native per commit: delete-region, replace-region, bounded regexp search with
truthful no-match/too-complex/cancel statuses, match data, and an undo-boundary
primitive only if its semantics are honest.

External positions stay 1-based codepoints and convert at the adapter seam;
core edits and markers stay byte-positioned.  Match state belongs to the
runtime execution context.  Marker objects and `save-excursion` wait for Plan
03 marker handles; `save-excursion` also waits for Phase 4 unwind.

Tests cover UTF-8/malformed bytes, stale handles/markers, hidden buffers,
read-only refusal before partial work, one intended undo, regex exhaustion and
cancellation, captures, and error rollback.

## Phase 4 — Implement Fe unwind before unwind-safe forms

Turn `fe/doc/unwind-design.md` into tested Fe cleanup records and Lisp
`unwind-protect`.  Host cleanup must run exactly once on ordinary return,
error, `C-g`, and budget exhaustion; cleanup failure has a defined diagnostic
without skipping remaining cleanups.  Test nested cleanup, roots, files, and
adapter-owned temporary state in Fe before moving the kg pin.

Only then implement:

- `save-excursion` with temporary markers;
- `with-current-buffer` restoring runtime context;
- true `atomic-change-group` after Plan 02 supplies a genuinely staged
  multi-edit or rollback contract;
- resource-binding helpers used by processes/packages.

## Phase 5 — Hooks through the typed event queue

After Plan 03, add deterministic global/buffer-local hook registries as one
runtime subscriber.  Each callback gets its own guarded Fe frame, budget,
label, handle re-resolution, and error isolation.  Callback-generated events
wait for the next drain.  A prompt-active safe point defers callbacks that
could prompt; nested minibuffers are rejected clearly.

Initial hooks: after-change, find-file, before-save, after-save, and major-mode
when its registry exists.  Do not add post-command until its every-keystroke
cost and semantics are measured.  Queue wakeup must return control from the
input wait to the main safe point; do not invoke Fe inside `read_key_byte()`.

## Phase 6 — Registry-dependent editor APIs

Land only when each named prerequisite exists:

- keymap APIs after Plan 01 layered maps and runtime command references;
- mode APIs after an explicit mode registry;
- buffer-local typed variables after a variable registry and per-variable
  file-local safety decision;
- `read-string`/`completing-read` after one minibuffer session replaces the
  duplicated loops;
- runtime-defined modes/maps after descriptors can own rooted callables.

Preserve emergency keys and `kg -Q`.  Keymap bindings use canonical parsed
sequences.  Mode activation performs no Lisp callback per displayed byte.
Completion candidates and roots are batched/bounded.

## Phase 7 — Bounded process table and Lisp process APIs

Extend `process.h` in two steps:

1. spawn request distinguishes explicit argv execution from intentional
   `/bin/sh -c`, with cwd/stdin/stdout/stderr and process-group policy;
2. a bounded process table owns PID/PGID, output descriptor, status, target
   buffer, queued bytes, filter/sentinel roots, and generation-checked handle.

Define slot reuse, how long terminal status remains queryable, output/queue
caps, truncation messages, process-buffer ownership, cancellation escalation,
shutdown cleanup, and root release.  Poll without selecting a buffer.  Commit
output through the internal live-edit gateway and emit bounded output/exit
events; filters/sentinels run only from the Phase 5 drain.

Expose explicit-argv `start-process`, separate `start-shell-command`,
process-live-p, delete-process, process-buffer, filter, and sentinel APIs.
Never expose a PID as identity.

Tests: argv without shell interpretation, cwd, process groups/grandchildren,
two concurrent children, output cap/queue overflow, cancellation, prompt idle
delivery, stale handles/PID reuse, sentinel error/queued sentinel, shutdown,
and both Lisp configurations.

## Phase 8 — Package hygiene and proof packages

Add bounded load-path, `provide`/`require`/`featurep`, docstrings,
documentation, load-cycle detection, and source byte offsets.  Publish a
versioned kg Lisp API document covering supported forms, object lifetimes,
position units, safe points/order, errors, limits, Emacs differences, and the
fact that init/packages are trusted code, not a sandbox.

Proof packages, in order:

1. whitespace mode: decorations + option + mode hook;
2. a small derived config/make mode: syntax/comment options + local map;
3. auto-fill: after-change + one transaction;
4. optional process-backed grep after Phase 7.

If a proof needs private C knowledge, improve the public adapter.  Each package
gets isolated-HOME PTYs and a `WITH_LISP=0` non-regression.

## Completion gate

- Direct Fe calls are option-budgeted; the trampoline is gone.
- Unwind-safe forms land only after tested Fe cleanup.
- Hidden-buffer runtime work never swaps active editor/window state.
- Every exposed object is generation checked and every root/process/fd has one
  release point.
- Runtime hooks/process callbacks run only through Plan 03 safe-point events.
- Explicit argv is not interpreted by a shell.
- Versioned API docs and at least two proof packages are green.
- Fe standalone tests/pin discipline, both kg Lisp builds, full native/PTY/
  sanitizer/static suites, and the full CI runner pass without ratchet raises.
