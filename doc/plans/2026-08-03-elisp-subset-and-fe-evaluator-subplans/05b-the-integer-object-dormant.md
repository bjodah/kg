# 05B — The integer object, dormant

Parent: [Phase 5](../2026-08-03-elisp-subset-and-fe-evaluator.md#9-phase-5--add-integers-and-complete-numeric-operations),
fe-only, behaviour-neutral — the 04B pattern: the representation lands
alone, priced and tested, read by nothing a program can reach.

**Prerequisite:** [05A](05a-pin-the-numeric-target-and-fund-the-phase.md).
The enum-placement Decision and the funded caps must exist first.

## Outcome

`FeTInteger` exists: constructible and readable from the host API,
printable, typed, collected, and **producible by no Lisp program** —
the reader is untouched and no primitive returns one.  Every golden,
compat comparison and test expectation is byte-identical; that is the
slice's correctness argument, verbatim from 03B/04B.

## Files this slice owns

**fe:** `fe.h` (`FeTInteger` per 05A's placement Decision;
`FeMakeInteger`, `FeToInteger` — additive, no version move *unless*
placement (a) renumbered the enum, in which case the bump still waits
for 05D and this slice simply notes the pending break in the header
comment), `fe.c` (`type_names[]` `"integer"` slot, the writer's
`FeTInteger` arm printing `%PRId64`, `FeToDouble` widened to accept an
integer — the parent's "matching conversions accept both types
afterwards"), `fe_internal.h` (`Value` gains `int64_t i`; an
`INTEGER(x)` accessor beside `DOUBLE(x)`), `fex.c` (`FexGC`'s
exhaustive switch gains the leaf arm), `test_api.c` (`TestInteger`),
`doc/implementation.md`.

**kg:** the gitlink in its own green commit; nothing else — no kg
figure moves (no new interned symbols, so `FeMinimumArenaSize()` is
unchanged; verify, don't assume).

## The work

1. **The union member.**  `int64_t i` in `Value` (`fe_internal.h:79`).
   Assert the invariant the whole design rests on:
   `static_assert(sizeof(Value) == sizeof(FeObject*))` — 05A's spike
   confirmed it, this makes it permanent.
2. **Constructors and accessors.**  `FeMakeInteger(ctx, int64_t)`
   mirrors `FeMakeDouble` (`fe.c:431-436`); `FeToInteger(ctx, obj)`
   mirrors `FeToDouble` with `CheckType(..., FeTInteger)`;
   `FeToDouble` additionally accepts `FeTInteger` and converts —
   that widening is what will make every kg `lisp_finite()` argument
   read work unchanged at 05E, and it is observable to no current
   program (nothing makes an integer yet).
3. **The writer.**  A `WriteObject` arm printing `%PRId64` via the
   checked `Format`.  The double path (`EmitDouble`) is **not**
   touched — its integral-shortcut death belongs to 05D's printer
   Decision, not here.
4. **Types and GC.**  `type_names[FeTInteger] = "integer"`; `FexGC`'s
   switch (no `default:`, ends in `abort()`) gains the leaf case; the
   collector needs nothing else — an integer marks exactly as a double
   does (a non-pair leaf).  If 05A chose placement (a), every
   renumbered constant is compile-checked by the existing
   `FeTFex0 > FeTPtr` assert and the exhaustive switches; build both
   core compilers and the Fex binary before trusting it.
5. **Arithmetic stays double.**  `ResumeArith`/`ResumeBinary`/the `=`
   arm are untouched; a host-made integer reaching them goes through
   the widened `FeToDouble` (arith) or fails the existing
   `FeTDouble` checks (`<`, `=`) exactly as any non-double does today.
   Both behaviours are dormant-state artifacts 05C replaces; do not
   polish them.

## Tests owned by this slice

`TestInteger` in `test_api.c`, following `TestSymbolCells`' pattern:
`FeMakeInteger` round-trips through `FeToInteger` (including
`INT64_MIN`/`INT64_MAX`); `FeGetType` says `FeTInteger`; the writer
prints `-9223372036854775808` and `42` exactly; `FeToDouble` of an
integer converts; a forced collection with a live integer preserves the
value; `(type-of ...)` is *not* testable from Lisp here (no producer)
— the `type_names` slot is asserted via the C surface instead.
Everything else is preservation: full `make -C fe check` (three passes,
goldens byte-identical), `make -C fe compat` unchanged, the 05A
`planned` entries still planned.

## Gates

fe: `make -C fe check`, `complexity-check`, `pmccabe-check` (inside
05A's funded caps; record measured before/after per Rule 6 — this
slice should be nearly free), `format-check`, `compat`.  The fuzz
smoke with tracked seeds — the reader cannot produce an integer yet,
but `fuzz_eval`'s `BuildNumber` is two lines from being able to
(`fuzz/fuzz_eval.c:47-50`); extending it to emit `FeMakeInteger` for
odd tag bytes is **this** slice's one reach-ahead, because it gives
every later slice a fuzzer that already exercises mixed types.

kg: pin move in its own green commit; `make check` and
`make WITH_LISP=0 clean all check` as the no-op confirmation.

## What this does not do

- **No reader or printer-of-doubles change, no arithmetic, no new
  primitives, no version move.**  05C and 05D.
- **No Lisp-visible behaviour change of any kind.**  `(type-of 1)` is
  still `double`; `42.0` still prints `42`.
- **No `FeTInteger` special-casing sprinkled ahead of need** — the
  tower adds type dispatch where the tower is, not here.
