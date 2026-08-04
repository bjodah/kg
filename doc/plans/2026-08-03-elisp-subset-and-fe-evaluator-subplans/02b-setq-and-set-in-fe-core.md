# 02B — Add core `setq` and ordinary-function `set`

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
Fe implementation first, then a kg gitlink update.

**Prerequisite:** [02A](02a-pin-the-target-semantics.md).  Its checked-in
oracle records, especially the lexical `set` case, are this slice's spec.

## Outcome and temporary state

Standalone Fe gains:

- `setq`, a special form which receives raw symbol/value forms;
- `set`, an ordinary function semantically (its symbol argument is evaluated),
  represented as a Fe primitive for a small implementation.

Assignment `=` remains reachable in this slice.  That temporary coexistence
is intentional: Fe's tracked sources can migrate to an already-tested `setq`
in 02C.  No release is cut between 02B and 02C.

When kg moves to the 02B Fe commit, kg's prelude still defines and shadows
`setq` with its existing macro.  That is also temporary and means kg behavior
must remain unchanged in this slice; 02D removes the shadowing macro only
after the `=` cut.

## Files to edit

### Fe implementation: `fe/fe.c`

Keep the change inside the existing primitive registry and
`EvaluatePrimitive()`:

1. Give the old assignment primitive an honest temporary enum name such as
   `PAssign`; keep `primitive_names[PAssign] = "="` unchanged.
2. Add `PSetq` with canonical name `"setq"` and `PSet` with canonical name
   `"set"`.  Do not add aliases.
3. Implement `PSetq` by iterating the raw argument list in pairs.  Check the
   target is a symbol before evaluating its value; evaluate values left to
   right; assign through `GetBound(ctx, symbol, env)`; retain and return the
   last value.  With no pairs, return `nil`.
4. Implement `PSet` with ordinary call semantics: require exactly two raw
   forms, evaluate both left to right (the existing `EvaluateList()` is the
   ordinary-call path), then validate the first resulting value as a symbol
   and call the existing public `FeSet(ctx, symbol, value)`.  `FeSet`
   deliberately looks up with `&nil`, so it writes the global cell and cannot
   accidentally mutate a lexical binding.  Return the second value.

This is the smallest shape that matches the oracle.  Do not add a second
environment lookup helper, change symbol layout, expose a new C API, or make
`set` call the `setq` arm.

`FeOpenContext()` and `GetCoreObjectCount()` already iterate the whole
primitive enum, so new registry rows are installed and included in the
minimum-arena calculation automatically.  Verify that rather than adding
parallel counts.

### Error text: local to the new forms

02A's supported error cases require Fe's message to contain the Emacs
condition name.  Existing helpers emit generic text (`too few arguments`,
`too many arguments`, `expected symbol`) and changing them globally would
alter unrelated APIs and golden tests.

Add the smallest local validation/helper needed so the new forms use messages
containing:

- `wrong-number-of-arguments` for odd `setq` input and every non-two-argument
  `set` call;
- `wrong-type-argument` when a `setq` target or `set`'s evaluated first value
  is not a symbol.

For `set`, reject a raw argument count other than two before evaluating either
argument.  For `setq`, process complete pairs left to right and diagnose a
dangling final symbol only when reached, so assignments from earlier pairs
stand as they do in Emacs.  Do not invent condition objects or copy Emacs'
full error-data formatting; Phase 6 owns that.

### Fe manifest and focused tests

- `fe/compat/features.json`: move the 02A `setq` and `set` rows from
  `planned` to `supported`, and set their `source_name` values to `"setq"`
  and `"set"`.  Leave both numeric-`=` and assignment-`=` rows unchanged.
- `fe/test_api.c`: add a focused test near `TestBinding`, or extend that test
  without burying the new contract.  Use the public evaluation API and the
  existing exact-error helpers.
- `fe/doc/language.md`: document the two new forms, their return values, and
  the lexical/global distinction.  Mark `=` assignment as transitional until
  02C rather than documenting coexistence as a compatibility promise.

After the Fe commit, update the two hard-coded inventory comments/docstrings
in kg's `utils/check_lisp_compat.py` from 31 primitives + one alias to
33 primitives + one alias.  The parser is already dynamic; this is prose
drift, not new logic.

No change belongs in `fe/fe.h`: neither construct is a C API addition.

## Native test matrix

The compatibility corpus proves agreement with Emacs.  `fe/test_api.c` should
add the implementation-focused properties the process-per-case corpus cannot
observe after an error:

- `(setq)` is `nil`; multi-pair `setq` returns the final value and later values
  observe earlier assignments.
- A lexical assignment changes the local cell and leaves an existing global
  cell unchanged.
- A global assignment creates a previously unbound value.
- On a value error, prior pairs remain assigned and later pairs do not run;
  the same context successfully evaluates another form afterwards.
- Odd `setq` input and a non-symbol target produce the named messages.
- `set` returns its value, writes the global cell through a same-named lexical
  binding, and rejects non-symbol and wrong-arity calls with the named text.
  An arity error evaluates no arguments; on a two-argument call whose first
  value is not a symbol, side effects from the second argument have already
  happened before the type error.
- Exact arity for these built-ins is the same with `FeSetStrictArity()` off
  and on; the lax-arity option still applies only to user functions/macros.
- A small regression assertion proves `(= old-name 7)` still assigns and
  still returns its old `nil` result in 02B.

Do not add a PTY test: these are pure standalone evaluator semantics and a PTY
would only add terminal timing to the same expressions.  Do not use a kg
native stub or load kg's prelude into Fe.

## Compatibility-corpus gate is separate from `make check`

`make -C fe check` currently runs `test_api`, the example host, and the script
suite; it does **not** run `make -C fe compat`.  Run both explicitly:

```sh
make -C fe compat
make -C fe check
```

The first command must show every 02A `setq`/`set` case as a pass, not a known
gap.  Numeric-`=` remains a known gap, and `primitive-assign-eq` remains the
intentional divergence.  The second command proves the ordinary Fe API and
all three script passes, including `fe -a`, are unchanged.

## Complexity hold point

The Phase 2 Fe estimate is +20 to +30 net of the deletion that occurs only in
02C.  The current Fe cap has six points of total headroom (214/220), so 02B may
cross it before repayment.

Run `make -C fe complexity-check` and `make -C fe pmccabe-check` before the
edit and again on the smallest complete implementation.  If either cap is
red, the junior implementer must stop with the measured total/file/function
deltas; they must not choose a new ratchet number themselves.  The maintainer
then records a dated Phase 2 Decision in this directory's README and approves
the smallest bounded raise.  Name 02C's deletion as a repayment candidate,
but do not promise a number before it is measured.

If the code fits, record a dated no-raise Decision instead.  Either way, do
not trade clear validation for a compressed high-pmccabe arm: the per-function
gate is independent of scc.

## Commit and verification sequence

1. Record both Fe and kg complexity gates from an idle tree.
2. Implement and test in Fe; run `make -C fe compat`, `make -C fe check`,
   `make -C fe complexity-check`, and `make -C fe pmccabe-check`.
3. Run Fe's full `.ci/run-ci-steps.sh` before the pin moves, because this is a
   language implementation change.
4. Commit the Fe change on `analyzers-etc`.
5. In kg, move the gitlink, update only the inventory-count prose noted above,
   and run `make lisp-compat-check`, `make check`, and
   `make WITH_LISP=0 clean all check`.
6. Run kg's `make complexity-check` and `make pmccabe-check`; the kg source
   score should not move in this slice.

The Fe commit message must say why two assignment spellings coexist and that
02C deletes the old one before any release.

## What this does not do

- It does not change numeric `=` or migrate any tracked assignment source.
- It does not introduce `FE_LANGUAGE_VERSION`; 02C versions the completed
  hard cut.
- It does not edit kg's prelude, generated include, filenames, loader, PTY
  fixtures, or user documentation.
- It does not redesign binding, environments, evaluation frames, arity policy,
  or conditions.

## Status

**Complete, 2026-08-04.**  Fe commit `fb3e536` on `analyzers-etc` (plus
`79a449f`, a documentation correction found in review); kg gitlink moved in
`fdd9f65` and `f5e75a2`.  Standalone Fe has `setq` and `set`; assignment `=`
still works, unchanged, and kg's behaviour is untouched — its prelude macro
still shadows the new core `setq`, as this slice intends.

**Shape.**  The old `PSet` enumerator (which spelled `=`) is renamed
`PAssign`, its arm unchanged; `PSetq` and `PSet` are new, the latter now
meaning Emacs' `set`.  That identifier reuse was the slice's main hazard and
it landed cleanly.  Two small static helpers before `EvaluatePrimitive()`
carry the logic — `EvaluateSetq()` (pmccabe 5) and `EvaluateSet()` (5) —
rather than inline switch arms, so `EvaluatePrimitive()` stays at its
existing 15 and validation is not compressed to buy a gate.
`FeOpenContext()`/`GetCoreObjectCount()` needed no edit: both already
iterate to `PSentinel`.  No new environment helper, no C API change, no
`fe.h` edit.  GC discipline is the surrounding arms': `MakeObject()` pushes
every new object onto the GC stack, `GetBound()` allocates nothing, and
`FeSet()` cannot allocate, so nothing is reachable-only through a C local
across an allocation.

**The semantics were checked against the binary, not just the suite.**  All
five worked examples in `doc/language.md` were re-run through `./fe` and
match 02A's pinned snapshots exactly, including the two that matter most:
`((lambda () (setq x 9) (list ((lambda (x) (setq x 2) x) 1) x)))` is `(2 9)`
and the same shape with `(set 'x 2)` is `((2 1) 2)`.  `make -C fe compat` is
66 cases, **43 passed** (up from 31), 23 known gaps, 0 failed — all 12
`setq`/`set` cases converted from gap to pass, with numeric `=` and
`primitive-assign-eq` still gaps owned by 02C.  Fe's full nine-stage
`.ci/run-ci-steps.sh` is green.  kg: `make check` 32/405, `make WITH_LISP=0
clean all check` 337 pass + 68 skip, `make lisp-compat-check` 183 features /
0 problems, now reporting 34 fe primitives/aliases.

### The complexity hold point was not reached — and the reason matters

Neither fe gate moved: **scc 214/220 total and `fe.c` 106/112 file, both
byte-identical before and after**, against a priced estimate of +20 to +30.
That is not the code being cheap.  Measured during review: **`pmccabe`'s
whole-file sum for `fe.c` rose 340 → 350** across the same diff, and
`pmccabe-check` recorded the two new symbols.  scc reported zero.

This is 00A's spike finding seen from the other side, and it is now
quantified.  `fe.c`'s first `'"'` character literal is at line 1010; scc's C
parser desynchronizes there and undercounts keywords afterwards.  All of
Phase 2's new code sits near line 1663 — **past the desync, therefore
invisible to scc**.  The consequence for the rest of this program:

- **`fe`'s scc ratchet cannot price work added below `fe.c:1010`.**  The
  price table's fe column is denominated in a unit that does not measure the
  file the work happens in.  It is a floor that happens not to move, not a
  budget that was respected.
- **02C must measure its repayment with `pmccabe`'s per-file sum**, not scc.
  Deleting `PAssign` will likewise show ~0 on scc; the honest number is the
  pmccabe delta from 350.
- Phase 3's split is where this stops being latent: extracting a quote-heavy
  region *raises* scc by tens of points with no behaviour change (00A
  measured +42), because it moves code back into the scanner's view.  Phase
  3's Decision should settle whether fe tracks a pmccabe sum ratchet
  instead.  That is not Phase 2's to add.

No cap was raised, no baseline-writing target was run, and no Decision was
needed — but "the gate was green" is the wrong summary of why.
