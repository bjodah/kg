# 05C — The numeric tower

Parent: [Phase 5](../2026-08-03-elisp-subset-and-fe-evaluator.md#9-phase-5--add-integers-and-complete-numeric-operations),
fe-only.  The phase's biggest fe spend.

**Prerequisite:** [05B](05b-the-integer-object-dormant.md).  The type
must exist, be priced, and be fuzzable before arithmetic dispatches on
it.

## Outcome

Every arithmetic, comparison and math-native path handles both numeric
types with the 05A-pinned Emacs semantics — integer-preserving where
Emacs preserves, promoting where Emacs promotes, truncating division,
`arith-error` on integer division by zero and int64 overflow,
variadic chained comparisons, the new comparator and predicate
primitives — while the **reader still produces only doubles**, so
`(+ 1 2)` still computes in doubles end to end and every existing
script, golden, and step pin is unchanged.  Integers flow only from
the host API and the fuzzer.  This is 04C's shape: the machinery
complete and tested through the seam that already exists, the cut
deferred to the next slice.

Two deliberate, observable exceptions — behaviour changes this slice
*does* make, because deferring them to 05D would mean touching the same
arms twice:

1. **Unary `-` becomes negation and unary `/` reciprocal** (05A cases
   A1/A2): `(- 5)` → `-5`, `(/ 5)` → `0` — wait: with a double-only
   reader, `(- 5)` is `(- 5.0)` → `-5.0`, printed `-5` by the old
   integral shortcut, and `(/ 5)` is `0.2`, printed `0.2`.  Audit
   fe's scripts for single-operand `-`/`/` first (none known); goldens
   must not move, and a golden that does move is a script relying on
   the old seed semantics — a finding, not a refresh.
2. **`<`/`<=` become variadic and chained** (C1): `(< 1 2 0)` flips
   from `t` (the old ignore-extras bug, `fe_eval.c:1345-1346`) to
   `nil`.  No tracked script passes three operands; verify.
3. **Numeric type errors say `wrong-type-argument`** (C5), retiring
   the `expected double, got X` texts on the numeric family only —
   `CheckType`'s general message survives everywhere else.

## Files this slice owns

**fe:** `fe_internal.h` (the `Primitive` enum: `PGreater`,
`PGreaterEqual`, `PNotEqual`, `PIntegerp`, `PFloatp`), `fe.c`
(`primitive_names[]` — `>`, `>=`, `/=`, `integerp`, `floatp`;
`GetCoreObjectCount` adapts; `Equal()`'s integer arm per 05A's `is`
Decision; the math natives' per-function return types), `fe_eval.c`
(the tower proper), `fe.h` (nothing — no API moves here), `test_api.c`,
`fuzz/fuzz_eval.c` (grammar: the new names), `compat/features.json`
(flip the 05A entries whose semantics are complete without the reader
— the arithmetic, comparison, predicate and math rows; the reader,
printer and equality rows stay `planned`), `doc/language.md`,
`doc/implementation.md`.

**kg:** the pin move in its own green commit.  The five new primitive
names must be claimed in fe's manifest **in the fe commit** —
`utils/check_lisp_compat.py` parses `primitive_names[]` at kg's
`lisp-compat-check` and unclaimed names fail the pin.  None of the
five collides with a kg-owned name (05A's table; `numberp` is
deliberately absent from this list).  `doc/fe-upstream.md`'s
minimum-arena figure moves with the pin (five new interned names,
predicted ≈96 bytes each; record the measured value).

## The tower, site by site

- **A numeric-pair helper, once.**  One internal function that takes
  two numeric operands and either returns both as `int64_t` (both
  integers) or both as `double` (anything else), raising
  `wrong-type-argument` on non-numbers.  Every binary site below uses
  it; do not re-derive the promotion rule per call site — that is how
  towers rot.
- **`ResumeArith`** (`fe_eval.c:1466-1501`): the accumulator becomes
  either-type; combine dispatches through the helper —
  integer+integer with `__builtin_add_overflow`/`sub`/`mul` raising
  `arith-error` on overflow (05A Decision 5), integer division
  truncating toward zero with a zero-divisor check, any double
  promoting the rest of the reduction.  `ArithResult`'s zero-operand
  identities become **integers** (`(+)` → integer 0, `(*)` → integer
  1) — invisible today through the old printer, pinned by 05A's
  snapshots after the cut.  The unary seeds change per the outcome
  note above.
- **`ResumeBinary`'s comparison arm** dies; `<`/`<=` join `>`/`>=`
  as chained EvalList-style forms sharing one comparator loop with the
  direction as data — the `PNumericEqual` arm (`fe_eval.c:1825-1844`)
  is the template, including its up-front zero-arity rejection.  `/=`
  is **binary-only** (C3): a third operand is
  `wrong-number-of-arguments`, not a chain.
- **Step accounting.**  `+`/`-`/`*`//` charge no step of their own
  today and `TestEvaluationControl` pins `(+ 1 2)` at exactly 4;
  keep it.  Moving `<` from the Binary frame to an EvalList-shaped
  frame changes *its* step profile — no exact pin covers `<` (05A's
  spike lists them all), but say so in the commit message and extend
  the per-state GC table if a new resume shape appears (prefer reusing
  `FeFrameEvalList` with a comparator arm: zero new frame kinds, the
  04C rule).
- **`=`** extends through the same helper: exact within integers,
  mathematical value across types (C4: `(= 3 3.0)` → `t`), NaN and
  signed-zero behaviour preserved (the existing pinned cases must pass
  unedited).
- **The predicates** `integerp`/`floatp` are `FeFrameUnary` leaves
  beside `boundp` — two lines each.
- **The math natives** (`fe.c:1308-1415`): per-function return types
  from 05A's M rows — `floor`/`ceiling`/`round`/`truncate` return
  integers (round half-even, M4), `expt` per its signature table (M2),
  the transcendentals stay float.  Arguments go through the widened
  `FeToDouble`.  **The Fex shadowing fact** (`main.c:198` installs
  Fex math *after* the core, so the standalone `fe` binary's `floor`,
  `ceiling`, `log`, `round`, `truncate` are Fex's one-argument
  versions): `test_api.c`'s `TestMathNatives` pins the *core* build
  and updates here; `scripts/math.fe`'s golden exercises the *Fex*
  overrides and must stay byte-identical — if it moves, Fex leaked a
  change it should not see.  Fex's own math (`fex_math.c`) is out of
  scope and untouched.
- **`Equal()`/`is`** gains its integer arm per 05A's Decision 2
  (recommended: mathematical value across int/float, epsilon retained
  for double/double).  `eq`/`eql` are **not** this slice (05A's
  collision table: they land with the cut).
- **`FeIsFunction`'s primitive table** gains rows for the five new
  primitives (all function-shaped).

## Tests owned by this slice

- `test_api.c`: a `TestNumericTower` suite driving everything through
  the **host API** (`FeMakeInteger` + `FeCall`/`FeEvaluate` on
  constructed forms), since the reader cannot spell an integer yet:
  int/int preservation, mixed promotion, truncating division and both
  `arith-error` paths (divide-by-zero, all three overflow operators at
  the int64 edges), chained comparisons in both directions, `/=`
  arity, `=` across types, predicate answers, per-function math-native
  types, context reuse after every error.  Force GC across the new
  resume states.
- The extended `fuzz_eval` grammar (05B's reach-ahead plus the new
  names) with tracked seeds replayed — overflow checks and the
  either-type accumulator are exactly the class of change the fuzz
  lane exists for.
- Preservation: all three `test.sh` passes byte-identical, the 03A
  frame-trace goldens unedited, `TestEvaluationControl` unedited,
  `make -C fe compat` green with the flipped rows.

## Gates

fe: `make -C fe check`, `complexity-check`, `pmccabe-check` (inside
05A's caps, measured before/after — Rule 6; this is the slice the +50
to +70 was mostly priced for), `format-check`, `compat`, fuzz smoke.

kg: pin move in its own green commit; `make check` (including
`lisp-compat-check` against the five new names) and
`make WITH_LISP=0 clean all check`.

## What this does not do

- **No reader or printer change.**  `42` still reads as a double and
  prints as `42`; `42.0` still prints as `42`.  05D.
- **No `eq`/`eql`**, per the collision table.  05D.
- **No version move** — the language changes so far (unary seeds,
  chaining, error names) ride 05D's single `FE_LANGUAGE_VERSION` bump;
  a language version per slice would be noise within one workstream.
- **No kg-side source change.**  05E, atomically with the cut pin.
- **No new frame kind unless priced** — the chained comparators should
  reuse the EvalList machinery; if they genuinely cannot, the frame
  kind's full cost (every exhaustive switch, the GC-per-state table)
  is written down before the choice.
