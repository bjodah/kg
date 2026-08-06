# Sub-plan 09C — A mark phase with a flat stack (fe-only)

Prerequisite: 09B (so the exhaustion tests exist before the collector is
rebuilt under them). fe-only; no kg edits, no pin move.

## The defect, precisely

`FeMark` (`fe.c:308`) recurses on `car` — one C frame (32 B at `-Os`, 64
ASan, 80 MSan) per nesting level; the `cdr` spine is already an iterative
loop. Measured: SIGSEGV at ~262 000 levels on an 8 MiB stack; any arena
≥ ~4.9 MiB can build that from pure Lisp (`fe -s 5242880` + a cons loop
segfaults today); kg's 1 MiB arena caps chains at 55 759 cells but the
editor still segfaults inside `FeMark` at `ulimit -s ≤ 1280`, and the MSan
lane's margin is 1.8×.

## Constraints (measured, 09A correction 2)

- `GcMarkBit` is bit 1 of `Value.c`; cons/other discrimination is bit 0
  (`fe_internal.h:132-136`). `static_assert(sizeof(Value) ==
  sizeof(FeObject*))` (`:501`) forbids widening a cell.
- Callers stay: `CollectGarbage` marks 8 context roots + the GC stack;
  `MarkCleanupRoots` and `FeMarkEvaluatorRoots` (6 calls per frame) iterate
  arrays and call the mark per root. Only the per-object walk changes.
- kg installs no `mark_fn` (`FeSetMarkFn` unused in kg); Fex's
  (`fe/main.c:214`) must keep working — the host callback must be callable
  from the new walk.
- 03E's lesson binds: *delete* the recursive path, do not wrap it. 03F's
  lesson: prefer removing a limit to relating two.
- Per-function cap: the 22-gate reads modified mccabe; `FeMark` is 4 today
  and a single-function state machine has 18 points of room, but split
  helpers before nearing the cap (08's `ReadCharacter` split precedent).

## Mechanics — decide in-tree, between:

1. **Pointer reversal (Deutsch–Schorr–Waite)**: constant space, needs a
   per-cell "which child am I in" state. With both low bits taken, encode
   the return path in the tag of the *reversed* pointer itself or use the
   mark bit progression (unmarked → car-in-progress → done). Must not
   disturb `FeGetType` on live reads — but the GC runs stop-the-world, so
   only the collector observes intermediate states. Sweep must see final
   tags.
2. **Explicit worklist**: an arena-external stack of `FeObject*` grown by
   `realloc`, spilling gracefully — simpler, but reintroduces an allocation
   inside the collector (the one place that must not fail); a fixed-size
   worklist + reversal fallback is a hybrid with the worst of both.

The A-slice does not pre-decide; the criterion is: zero allocation inside
collection, bounded C-stack for *any* data shape, and the sweep/`FeGetType`
invariants provably restored. Whichever loses is recorded with the measured
reason in the commit body.

## Tests owned by this slice

- The 03E stack-probe convention transfers: a `mark-depth` probe measuring
  C-stack high-water across a collection of a deep-`car` structure at
  N = 10, 1 000, 100 000 — delta flat (< 2048 B), the same assertion shape
  as `TestEvaluationStackProbe` (`test_api.c:4537`).
- Deep-`car`, deep-`cdr`, alternating, and *cyclic* structures collected
  correctly (build a cycle via `setcdr`-equivalent C API, mark, sweep,
  verify liveness and tag restoration).
- `FeGetArenaStats` figures unchanged for the standard corpora
  (collection_count, peak_live identical before/after the rewrite on the
  same inputs) — the rewrite is invisible except to the C stack.
- Fex's `mark_fn` path exercised (fe's own suite covers Fex; confirm the
  host-pointer branch runs under the new walk).
- Fuzz: raise `fuzz_eval` `MaxDepth` 4 → enough to matter with a larger
  fuzz arena variant, add tracked deep-`car` and cycle seeds under
  `fuzz/seeds/`, reachability counts in the commit body (09A Decision 6).
- Sanitizer lanes are the real gate: ASan/MSan run the same probes at
  64/80 B per frame — the flat-stack assertion is what protects them.

## Gates

- Full nine-stage fe runner green.
- `tests/*.fe.out` goldens byte-identical (the rewrite changes no output).
- scc/pmccabe inside the 09B-raised caps; actuals recorded per slice.

## Price

fe +20..35 scc. The comparable is 03E's focused rewrite (~0 net after
deletion) *plus* a state machine 03E did not need; the band accounts for
the encoding work pointer reversal needs under the two-bit constraint.

## Explicitly not this slice

No printer/reader/`Equal` work — already bounded (09A correction 3). No
arena growth or new stat fields. No kg edits, no pin.
