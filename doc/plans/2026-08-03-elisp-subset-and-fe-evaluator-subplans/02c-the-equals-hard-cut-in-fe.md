# 02C — Cut Fe from assignment `=` to numeric `=` and version the language

Parent: [Phase 2](../2026-08-03-elisp-subset-and-fe-evaluator.md#6-phase-2--hard-cut-assignment-and-numeric-),
Fe-only implementation and migration.  kg adapts in 02D.

**Prerequisite:** [02B](02b-setq-and-set-in-fe-core.md).  Core `setq` must
already be supported by the oracle corpus before any old assignment call site
moves to it.

## Outcome

Fe ends this slice with exactly one assignment spelling (`setq`) and `=` as
left-to-right chained numeric equality over the existing double type.  The
old assignment primitive and its compatibility-manifest row are gone, and the
language break is visible through an explicit version constant.

Fe's own `.fe` filenames do **not** change.  They name Fe's standalone
dialect artifacts; kg's `.el` filename cutover is 02D.

## Corrected migration inventory

The previously quoted “99 Fe sites” counts only occurrences under
`fe/scripts/*.fe`.  The implementation would still break if a junior stopped
there.  Recount by symbol before editing; in the audited tree the tracked
surface is:

| Path | Assignment-shaped occurrences and action |
|---|---|
| `fe/scripts/*.fe` | 99; migrate executable forms, quoted macro expansions, and the commented example |
| `fe/test_api.c` | 53 C-string forms; migrate, and update the few assertions which deliberately observed assignment's old `nil` return |
| `fe/example_host.c` | 1 embedded example |
| `fe/fuzz/fuzz_eval.c` | 1 evaluator preamble |
| `fe/fuzz/fuzz_write.c` | 1 generated assignment prefix used to construct writer inputs |
| `fe/compat/cases/writer-bounded-output.json` | 1 setup inside an unrelated supported writer case; migrate it so that case keeps testing the writer |
| `fe/compat/cases/primitive-assign-eq.json` and matching oracle | delete, do not migrate; they exist only to describe the retired divergence |
| `fe/doc/language.md` | 14 assignment examples/headings in the audited tree; rewrite the language reference for `setq`, `set`, and numeric `=` |

`fe/tests/*.out` and `fe/tests/*.err` contain no assignment forms in the
audited tree, though their expected output must still be reviewed after the
scripts move.  Do not claim those files account for the 99.

## Two Fe commits, both green

Use two commits rather than the old three-commit sequence.  The language
version belongs in the same commit as the break; otherwise the precise
runtime mismatch it is meant to expose exists for one commit without a
version check.

### Commit 1 — migrate Fe-owned callers while old `=` still exists

Mechanically change assignment uses in the paths above to `setq`.  Pay
attention to strings and quoted code, not just parsed top-level forms:

- `scripts/macros.fe` contains a quoted expansion that must produce `setq`;
- the fuzz writer constructs source text in pieces;
- `test_api.c` contains source inside C strings;
- the `writer-bounded-output` compatibility case is owned by a writer feature,
  so leaving old assignment syntax there would make an unrelated supported
  feature fail after the cut.

Most script outputs are behavior-neutral because they discard assignment's
return.  A few `test_api.c` checks expect `nil` from old `=`; core `setq`
correctly returns the assigned value, so update those expectations explicitly
and call that small semantic test change out in the commit message.  Do not
pretend the text substitution is byte-for-byte behavior-neutral.

Run `make -C fe check` and `make -C fe compat` on this coexistence state.

### Commit 2 — cut, manifest transition, documentation, and version

Make these changes atomically:

- `fe/fe.c`: delete the temporary old-assignment enum/registry/switch arm;
  add or rename the `=` primitive to an honest numeric-equality enum; bump
  `FeVersion` from `1.1` to `2.0` for the breaking language release.
- `fe/fe.h`: add `#define FE_LANGUAGE_VERSION 2` beside
  `FE_API_VERSION 1`.  Version 2 names the first explicit contract while
  acknowledging the shipped assignment-`=` dialect as implicit language 1.
  `FE_API_VERSION` remains 1 because no C function, type, or callback contract
  breaks.
- `fe/example_host.c`: assert both `FE_API_VERSION == 1` and
  `FE_LANGUAGE_VERSION == 2`, demonstrating what downstreams must do.
- `fe/doc/c-api.md` and `fe/README.md`: distinguish embedding-API version
  from language version and tell downstreams to assert both.
- `fe/doc/language.md`: make `setq`/`set` the assignment reference and `=`
  the numeric comparator; remove prose that presents old assignment `=` as
  current behavior.
- `fe/compat/features.json`: move numeric `=` from `planned` to `supported`
  and give it `source_name: "="`; delete `primitive-assign-eq`.
- delete `fe/compat/cases/primitive-assign-eq.json` and its oracle snapshot,
  so the manifest checker sees no orphan describing retired behavior.

The version values above remove a design choice from the implementer's task.
They also resolve an over-broad sentence in the parent's cross-repository
checklist which says every semantic change updates `FE_API_VERSION`: for this
language-only cut, “update” means review and document the unchanged API value,
not increment it and falsely advertise a C embedding break.  This slice's
specific API/language split controls.
Phase 5 increments `FE_LANGUAGE_VERSION` to 3 when integers extend the numeric
contract; it does not redefine version 2.

## Numeric `=` implementation shape

Do not copy the existing two-argument `NUM_CMP_OP` arm verbatim; it accepts
the wrong arity shape, does not chain, and can leave extra operands unevaluated.
The new arm must:

1. reject zero arguments with a message containing
   `wrong-number-of-arguments`;
2. evaluate the complete raw argument list left to right with the existing
   `EvaluateList()` ordinary-call path before validating any numeric type;
3. validate the first value as a double and return `t` for one argument;
4. validate and compare every remaining value, accumulating equality without
   short-circuiting;
5. return `t`/`nil` only after every operand form has run;
6. use a local numeric validator whose error contains
   `wrong-type-argument`, rather than globally changing `FeToDouble()` or
   `CheckType()` and rewriting unrelated diagnostics.

Plain C double comparison gives the pinned signed-zero and NaN answers.  Do
not special-case them.  Preserve the evaluated numeric object on Fe's GC
stack or in already-rooted state using the same discipline as the surrounding
primitive arms.

## Tests owned by this slice

### Oracle compatibility

Run `make -C fe compat` separately from `make -C fe check`.  Every numeric-`=`
case from 02A must now pass, including unary true, chained false with a later
side effect, signed zero, NaN via `sqrt`, both wrong-type cases, and the
unbound-symbol proof that `=` no longer assigns.

### Native evaluator regression

Extend the focused Phase 2 test in `fe/test_api.c` with:

- zero-, one-, two-, and three-argument behavior;
- a false comparison whose later operand mutates a counter, proving no
  short-circuit;
- `0.0 == -0.0` and NaN unequal to itself;
- named wrong-arity and wrong-type error messages followed by successful
  context reuse; a wrong type in the first value still leaves a later
  operand's assignment visible, proving argument evaluation preceded type
  validation;
- `(= never-bound 3)` raising `void-variable` and leaving `never-bound`
  unbound;
- `setq` and `set` still passing after the old assignment arm is removed.

The 02A oracle tests are semantic authority; these native tests protect Fe's
error recovery and implementation path without spawning a process per
assertion.

### Migrated suites and fuzz harnesses

Run all three script passes through `make -C fe check`, including `fe -a`.
Because both fuzz source harnesses change, also build and smoke the focused
targets before full CI:

```sh
make -C fe fuzz-eval-smoke fuzz-write-smoke
```

Do not add or rewrite fuzz seeds merely because the bootstrap spelling
changed; seeds are byte inputs and the fuzz property is crash/resource safety,
not a golden printed result.  Change a tracked seed only if replay demonstrates
that it encodes a deliberate language example which must remain reachable.

## One-time audit, not a new checker

Before commit 2, search the **tracked** Fe paths above for `(` followed by `=`
and classify every survivor.  After commit 2, survivors should be numeric
tests/examples only.  Also search `PAssign`, the old enum spelling, and
`primitive-assign-eq` to prove the implementation and manifest divergence are
gone.

Record the command and classified survivors in the commit message.  Do not
check in the search as a script or CI target: after this cut `(= symbol value)`
can be a legitimate numeric comparison between two variables, so syntax alone
cannot decide whether it is stale assignment.

## Complexity repayment and gates

Measure scc and pmccabe before commit 1, after commit 1, and after commit 2.

**Use `pmccabe`'s per-file sum for `fe.c`, not scc, as the repayment
number.**  02B measured this: its whole diff moved `fe.c`'s pmccabe sum
340 → 350 and moved scc by exactly zero, because scc's parser
desynchronizes on `fe.c`'s first `'"'` literal (line 1010) and Phase 2's
code lives past it.  Deleting `PAssign` will show ~0 on scc for the same
reason.  Record the pmccabe delta from 350, say plainly that scc did not
move, and do not report a zero-scc result as evidence the cut was free.
See 02B's Status for the full finding.
If 02B required a reviewed cap raise, state exactly how many total/file/
function points the deleted arm returned.  Lower a cap only when the final
measurement provides durable headroom; otherwise record why the named
repayment was smaller than estimated.  Do not run a baseline-writing target
to hide an overrun.

The Fe workstream is complete only when all of these pass from an idle tree:

```sh
make -C fe compat
make -C fe check
make -C fe complexity-check
make -C fe pmccabe-check
(cd fe && .ci/run-ci-steps.sh)
```

Do not assert kg's language version in this Fe slice: the parent tree still
points at the earlier Fe commit until 02D.  02D adds kg's assertion in the
same commit that moves the gitlink and adapts all callers.

## What this does not do

- It does not edit kg's prelude, kg tests, kg documentation, or the gitlink.
- It does not rename Fe's `.fe` scripts.
- It does not add integers, number/marker coercion, `arith-error`, structured
  conditions, an assignment alias, or a deprecation warning.
- It does not add a permanent migration linter or a second evaluator mode.

## Status

**Complete, 2026-08-04.**  Two Fe commits on `analyzers-etc` as specified —
`9fe0220` (migrate every Fe-owned caller while assignment `=` still worked)
and `841df63` (the cut, the version, the manifest transition and the
documentation, atomically) — plus `5cc0acb`, a manifest-prose fix found in
review.  Fe now has exactly one assignment spelling.  **kg's gitlink was
deliberately not moved**; that is 02D's, and the reason is below.

**Numeric `=`.**  `EvaluateNumericEqual()`, a static helper beside 02B's two,
with a local `CheckNumericEqualOperand()` emitting `wrong-type-argument`
rather than `CheckType()`'s generic text.  Zero arguments are rejected before
anything is touched; `EvaluateList()` then evaluates the complete raw list
left to right before any value is type-checked; the loop validates *and then*
compares each remaining operand, so the `&&` short-circuits only the
side-effect-free `first == next`, never the type check.  Plain C `==`, with
the `#ifdef __clang__` `-Wfloat-equal` push/pop that `IsNearlyEqual()` already
established.  `FeVersion` is `"2.0"`; `FE_LANGUAGE_VERSION 2` is in `fe.h`
beside an unchanged `FE_API_VERSION 1`, because no C contract broke.

**Verified against the binary, not only the suite.**  All ten of 02A's pinned
answers reproduce exactly through `./fe`: `(= 1)`→`t`, `(= 1 1 1)`→`t`,
`(= 1 1 2)`→`nil`, `(= 0.0 -0.0)`→`t`, NaN via `sqrt` →`nil`, `(=)`→
`wrong-number-of-arguments`, `(= 1 "1")` and `(= 1 nil)`→`wrong-type-argument`,
`(= never-bound 3)`→`void-variable`, and the evaluation-order expression →
`(nil 3)` — the one that proves operands still run after the chain is known
false.  `make compat` is **65 cases, 53 passed, 12 known gaps, 0 failed**
(66→65 because `primitive-assign-eq` and its snapshot are deleted).  Fe's
full nine-stage `.ci/run-ci-steps.sh` is green, as are `check`, `compat`,
`complexity-check`, `pmccabe-check` and `format-check`.

**The migration found a trap the plan's inventory could not have named.**
`scripts/macros.fe`'s `++` and `push` build their expansions
*programmatically* — `(list '= sym ...)`, not literal `(= ` text — so no text
search reaches them.  Left alone they would still have parsed in commit 1 and
then silently stopped advancing their counter the moment commit 2 landed,
turning that file's own `(while (< i 10) ... (++ i))` into an infinite loop.
Both were fixed by hand to build `'setq`.  This is the concrete case for the
plan's insistence on classifying survivors rather than running a
search-and-replace.  A second trap the plan's table predates: 10 of the 13
`(= ` occurrences under `compat/cases/` are 02A's numeric-equality cases —
they are the *spec* for this slice and had to survive untouched.

Audit survivors after the cut (`git ls-files -z | xargs -0 grep -n '(= '`)
are numeric-equality material only: 02A's nine case files plus the
zero-arity oracle snapshot, `doc/language.md`'s new `(= number ...)`
section, and `test_api.c`'s numeric test.  `PAssign` and
`primitive-assign-eq` survive only inside `features.json` rationale prose
describing the retirement — and one such string, on the `primitive-setq`
row, still claimed the coexistence was live; `5cc0acb` corrects it.

**Complexity, measured the way 02B's finding requires.**  `pmccabe`'s
`fe.c` sum went **350 → 350 (commit 1 touches no C) → 356**, a net **+6**
for deleting an inline switch arm and adding two named helpers, against a
priced +20 to +30 for the whole of Phase 2.  scc did not move at all —
214/220 total, `fe.c` 106/112 file — and that is the blind spot, not a
refund: the deleted arm and the added helpers all sit past `fe.c:1010`.
The named repayment therefore did not materialise as scc headroom and no
cap was lowered.  `pmccabe-check` records 4 new symbols across 02B and 02C
with 0 gone and 0 improved, worst function still `EvaluatePrimitive` at 15.

**The intermediate state is real, and was observed.**  With Fe cut and kg's
gitlink still at the pre-cut commit, kg's working tree builds against the new
Fe while `lisp/prelude.fe`'s 54 top-level `(= NAME VALUE)` forms have become
numeric comparisons whose results are discarded.  `src/kg` still builds and
still reports `+lisp`, and a Lisp PTY case
(`111-lisp-string-natives.yaml`) fails with an empty result where the
prelude's definitions should be.  This is exactly the "builds but
misbehaves at startup" state 02D's atomicity requirement and
`FE_LANGUAGE_VERSION` exist to reject, confirmed rather than assumed.  **kg
is knowingly red between this slice and 02D**, and 02D must land as one
commit.
