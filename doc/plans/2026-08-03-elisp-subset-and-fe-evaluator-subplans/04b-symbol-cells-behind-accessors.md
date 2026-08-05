# 04B — Symbol cells behind accessors, with a dormant function cell

Parent: [Phase 4](../2026-08-03-elisp-subset-and-fe-evaluator.md#8-phase-4--adopt-lisp-2-namespaces),
fe-only, behaviour-neutral.

**Prerequisite:** [04A](04a-pin-the-lisp2-target-and-fund-the-phase.md).
The layout this slice implements is the one 04A's Decision recorded with
measured arithmetic; this slice is an implementation, not a design
exercise.

## Outcome

Every place that knows a symbol's cons layout goes through a named
accessor, and the layout gains the function cell — present, marked by the
collector, priced in the arena arithmetic, and **read by nothing**.  This
is the parent plan's Fe step 11 ("refactor symbol internals behind
accessors") plus the storage half of step 12, landed alone so that 04C and
04D change *lookup rules* against a stable representation, and so this
slice's correctness argument can be "every golden byte-identical, every
existing test expectation unchanged" — the same argument 03B used.

## Files this slice owns

**fe:** `fe_internal.h` (accessor declarations), `fe.c` (`FeMakeSymbol`,
`GetStringObject`, `IsNamedSymbol`, the writer's `FeTSymbol` arm, the
`fn`-alias registration, `GetBound`/`FeSet`/`FeIsBound`,
`GetSymbolObjectCount`, `GetCoreObjectCount`, arena-layout comments),
`fe_eval.c` (the `GetBound` callers: head/variable lookup in
`RunEvaluationLoop`, `ResumeSetq`, `boundp`/`makunbound` in `ResumeUnary`),
`test_api.c` (one new test; `TestContextCreation`/`TestArenaStats`
re-verified), `doc/implementation.md`.

**kg:** the `fe` gitlink in its own green commit, plus
`doc/fe-upstream.md`'s minimum-arena figure (53840 → the measured new
value) — a number that document pins explicitly.

## The accessors

Per the parent plan: do not leave direct `car`/`cdr` knowledge of symbols
distributed through the evaluator.  Introduce, in `fe_internal.h`, over
the 04A layout (`CDR(sym) = ((name . function) . value)` if the Decision
confirmed candidate (a)):

```c
FeObject* SymbolName(FeObject* sym);          // the name string chain
FeObject* SymbolBindingCell(FeObject* sym);   // the cell GetBound's global path returns
FeObject* SymbolFunction(FeObject* sym);      // &unbound when no function binding
void SetSymbolFunction(FeObject* sym, FeObject* fn);
```

Naming follows the existing internal style (`GetBound`, `GetStringObject`);
match it rather than inventing a public-API prefix — these are
`fe_internal.h` symbols, not `fe.h` ones.  `SymbolValue`/`SetSymbolValue`
are deliberately *not* added: the value path's unit of currency is the
binding **cell** (lexical env entries and the global cell share the
`CDR(cell)` read/write contract), and wrapping it in a value-shaped
accessor would hide exactly the symmetry `GetBound` depends on.

Order of work inside the slice, two commits:

1. **Accessors over the current layout.**  Replace every raw
   `CAR(CDR(sym))`/`CDR(sym)` symbol access at the sites listed above.
   `SymbolFunction` does not exist yet.  Pure motion; every test and
   golden unchanged.
2. **The layout change.**  `FeMakeSymbol` builds the new shape and
   initializes the function cell to `&unbound`; the accessors' bodies
   move; `GetSymbolObjectCount` becomes `5 + (len-1)/7`;
   `GetCoreObjectCount()`/`FeMinimumArenaSize()` shift by 04A's measured
   figures.  Nothing outside the accessors changes.

If 04A's Decision chose a different layout, the same two-commit shape
holds; only commit 2's bodies differ.

## What must not move

- **GC.**  With layout (a), `FeMark`'s `FeTSymbol` arm needs no edit — the
  new inner pair is reached through the existing cdr walk.  Verify rather
  than assume: force a collection with live symbols carrying function
  cells (set one via `SetSymbolFunction` from the new test) and assert the
  function object survives.  If the chosen layout is *not* reachable from
  the cdr walk, the mark arm changes here, and the 03F lesson applies in
  full: a green `make check` plus three sanitizer lanes is not evidence a
  GC-rooting change is safe — run fe's fuzz lane (`ci-06`) before
  believing it.
- **The writer.**  A symbol prints its name; `(env)` prints the intern
  list.  Both go through `SymbolName` after commit 1 and are therefore
  layout-blind.  Every `tests/*.out`/`*.err` golden stays byte-identical.
- **The step budget.**  `GetBound` charges `EvaluationStep` per env link;
  the accessor refactor must not add or remove charges — 03A's
  `frame-trace-*.fe` goldens and `TestEvaluationControl`'s exact step pins
  are the regression net, unedited.
- **Public API and versions.**  `fe.h` is untouched.  No version moves;
  a dormant private cell is not a contract.

## Arena arithmetic, re-verified

04A predicted the numbers; this slice lands them and re-verifies the
consumers:

- `FeMinimumArenaSize()` moves by the +1-object-per-core-symbol cost
  (predicted ≈54656 bytes).  `TestContextCreation`'s byte-boundary probe
  and `TestArenaStats`'s exact-fit case adapt by construction — confirm
  they pass unedited, and treat an edit as a finding.
- fe's 64 KiB fuzz arena and `TestArenaSize` fixtures: re-run
  `make -C fe check` and the fuzz smoke (`fuzz-seed` + smoke targets) so
  the tightest arena's headroom loss is observed, not assumed.
- kg's 1 MiB arena: frame capacity (1100) is untouched; object slots at
  open drop by the measured ~50.  kg's `test_perf` asserts margin
  *shapes*, not slot literals, so no kg test should move — if one does,
  that is the pin commit's finding to record.

## Tests owned by this slice

- One new `test_api.c` case (`TestSymbolCells`, following `TestBinding`'s
  pattern): interning still deduplicates; a symbol's printed name is
  stable; `SymbolFunction` of a fresh symbol is unbound;
  `SetSymbolFunction` roundtrips and survives a forced collection; the
  value cell is untouched by function-cell writes.  This is the only place
  the dormant cell is exercised until 04C.
- Everything else is preservation: full `make -C fe check` (script goldens
  byte-identical across all three passes), `TestEvaluationControl`,
  the 03A trace goldens, `make -C fe compat` unchanged.

## Gates

fe: `make -C fe check`, `complexity-check`, `pmccabe-check` (inside 04A's
funded caps — record measured before/after per Rule 6), `format-check`,
and — because commit 2 touches allocation-adjacent code — the fuzz smoke
with the tracked seeds replayed.

kg: pin move in its own green commit with the `doc/fe-upstream.md`
minimum-arena figure updated; `make check` and
`make WITH_LISP=0 clean all check` as the no-op confirmation.

## What this does not do

- **No lookup change.**  Head resolution and variable reference still go
  through `GetBound` exactly as today; the function cell is written by one
  test and read by nothing else.
- **No new primitives, no reader change, no public API.**  04C and 04D.
- **No behaviour change observable from Lisp.**  `(boundp 'car)` is still
  `t`; every compat comparison unchanged.
- **No layout re-litigation.**  If implementation contradicts 04A's
  Decision (a measurement was wrong, an invariant was missed), stop and
  amend the Decision with the new numbers — do not improvise a different
  layout mid-diff.
