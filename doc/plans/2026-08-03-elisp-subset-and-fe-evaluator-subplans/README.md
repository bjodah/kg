# Sub-plans — Emacs Lisp subset and Fe evaluator evolution

Parent plan:
[../2026-08-03-elisp-subset-and-fe-evaluator.md](../2026-08-03-elisp-subset-and-fe-evaluator.md),
reviewed and corrected 2026-08-04; each sub-plan set since has been
implementation-audited against the tree it starts from, and this Phase 5
set was written against the tree as it stands after Phase 4 closed and
its post-close review fixes landed (2026-08-05).  Read the parent's §0
(verified baseline), §0.1 (the two complexity ratchets), §0.3 (scope
honesty) and §0.4 (no legacy constituency) before any of these; they are
the facts these documents assume.  Where the parent's Phase 5 section
names stale facts — "ten call sites" where the tree funnels 13 natives
through one `lisp_position()` constructor and every numeric argument
through `lisp_finite()`, the `ARITH_OP`/`NUM_CMP_OP` macros that died
with the recursive evaluator in 03E, and §0.2's fe.c split that 03B
settled — 05A's corrections control.

Measured on the audited tree, 2026-08-05 (post-review), and re-measured
rather than carried forward per Rule 6: kg **5450/5500** scc, **32
native / 406 PTY** (**337 pass + 69 skip** under `WITH_LISP=0`), **70**
Lisp PTY cases; fe **459/480** scc with `fe.c` at **73** and `fe_eval.c`
at **276/300**, pmccabe **660/690 across 248 symbols** (worst functions
`DispatchPrimitive` and `RunEvaluationLoop` at 14), and
`FeMinimumArenaSize()` **55616 bytes**, kg's 1 MiB arena partitioning to
**1098 frames / 56221 object slots**.

**The first set — Phase 0 and Phase 1's extraction — is complete
(2026-08-04).**  Its five documents (`00a`–`00d`, `01a`) were removed once
they landed; the Status section at the foot of this file is the surviving
record, and the commits it names carry the detail.  What that set built is
the ground this one stands on: an oracle mechanism, a 180-entry
two-repository feature inventory with a checker in `make check`, recorded
performance and arena baselines, a priced per-phase complexity table with
a dated Decision, and a prelude that is a Lisp file rather than three C
string literals.

**The second set — Phase 2's hard cut — is complete (2026-08-04).**  Its
four documents (`02a`–`02d`) were removed once they landed; the Phase 2
Status section below is the surviving record.  What that set changed is the
ground this one stands on: `setq` and `set` are core Fe forms, `=` is
chained numeric equality, `FE_LANGUAGE_VERSION` is 2, and kg's Lisp is
`.el`.

**The third set — Phase 3's frame machine, plus Phase 2's residual debt —
is complete (2026-08-05).**  Its seven documents (`02e`, `03a`–`03f`)
were removed once the workstream was accepted; the Phase 3 Status section
below is the surviving record.  What that set changed is the ground this
one stands on: the recursive evaluator is gone, Lisp nesting consumes
frames in the context-owned arena and a constant amount of C stack, the
evaluator lives in `fe_eval.c` behind a private `fe_internal.h`,
`FE_API_VERSION` is 2, and pmccabe's funded total is the authoritative
complexity measure for the core.

**The fourth set — Phase 4, Lisp-2 namespaces — is complete
(2026-08-05).**  Its five documents (`04a`–`04e`) were removed once the
workstream was accepted; the Phase 4 Status section below is the surviving
record.  The set separated variable bindings from callable definitions:
every interned symbol gained a function cell, call position resolves
through it, `#'x` reads as `(function x)`, and `funcall`/`apply`/
`fboundp`/`symbol-function`/`fset`/`fmakunbound`/`defalias` are real.  It
was the first *language* cut since Phase 2, and it was a two-repository
cut: fe changed what a symbol is, and kg's prelude — 53 definitions, all
written into value cells — was rewritten against it.  What the set left
behind is the ground later phases stand on: `FeDefineNative` writes the
function cell, `FE_API_VERSION`/`FE_LANGUAGE_VERSION` are 3, and the
prelude's 52 definitions are spelled `defalias`.

**This set — Phase 5, integers and the numeric surface — is next.**  Its
five documents (`05a`–`05e`) are in this directory; the Grouping and
Sequencing sections below describe them, and `05a` must land before any
of the rest.  Phase 5 closes milestone 1, which carries an obligation the
price table wrote down for itself: when 05E closes the phase, the
provisional milestone-2 rows (Phases 6–9) are re-priced against real
measurements before any Phase 6 sub-plan is written.

The through-line is that Phase 5 is a *text-changing* phase, which
inverts Phase 4's discipline.  Four facts, established by auditing this
tree, shape the slices:

- **The phase deliberately changes what programs print**, so "no
  expectation edits" — Phase 4's red-flag rule — becomes "only the
  *enumerated* expectation edits, each justified by a pinned oracle
  answer".  The audit found the full allowlist in advance: six lines of
  one PTY case (`lisp-math-functions.yaml`), one `test_lisp.c` breakage
  table dominated by the `(/ 1 0)` family, one compat flip
  (`reader-no-integers`, whose snapshot has recorded the target since
  00C), and 150 000 lines of fe's `mandelbrot` golden that must **not**
  change because the script migrates to explicit float spellings
  instead.  An edit outside the list is a finding.
- **kg's numeric boundary is a funnel, not a scatter.**  13
  position-returning natives share `lisp_position()`
  (`src/lisp_buffer.c:181`), every numeric argument goes through
  `lisp_finite()`, and exactly three `FeGetType == FeTDouble` checks
  exist in all of `src/` (two `format` gates, `numberp`).  The kg
  cutover is smaller than the parent plan feared, and 05E's checklist is
  written from the funnel.
- **The manifest disjointness rule dictates slice boundaries.**  Every
  fe primitive, kg native and prelude definition name must be claimed
  exactly once across both manifests, so a new fe primitive whose name
  kg currently owns cannot land before the kg-side deletion: `eq` (a
  prelude alias of `is` today) lands with 05D's cut, `numberp` (a kg
  native) must never become an fe primitive at all, and the collision
  table in 05A is the authority.
- **Both fe gates are nearly spent again**: 23 scc and 32 pmccabe points
  of headroom against a row priced +50 to +70, so the funding Decision
  comes first, for the fourth time.

So the set front-loads one oracle-and-decision slice (05A: ~40 predicted
answers, five Decisions, the funded raise), lands the dormant
representation (05B), builds the whole tower against the host-API seam
while the reader still speaks only doubles (05C), cuts reader, printer,
`eq`/`eql` and the versions in one fe-only slice (05D), and lands kg's
pin, funnel edits, equality rewrite, enumerated expectations and
documents in one atomic commit that also closes the milestone (05E).

## Grouping

| Sub-plan | Phase | Focus | Prerequisites |
|----------|-------|-------|---------------|
| [05A](05a-pin-the-numeric-target-and-fund-the-phase.md) | 5 | The numeric oracle corpus (~40 cases), five Decisions — representation/enum placement, the equality-family ownership table, reader grammar, printer representation, error policy — and the funded cap raise | none — **this is first** |
| [05B](05b-the-integer-object-dormant.md) | 5 | `FeTInteger` exists: host-constructible, printable, typed, collected, producible by no Lisp program | 05A |
| [05C](05c-the-numeric-tower.md) | 5 | Integer-preserving arithmetic, truncating division, `arith-error` on overflow/zero-divide, chained `<`/`<=`/`>`/`>=`, binary `/=`, `integerp`/`floatp`, per-function math-native return types — all against a reader that still speaks only doubles | 05B |
| [05D](05d-the-numeric-cut.md) | 5 | The cut: Emacs number lexing replaces `strtod`, floats print `42.0`/shortest-round-trip/nonfinite spellings, `eq`/`eql` land, scripts migrate, `FE_LANGUAGE_VERSION` 4 and `FE_API_VERSION` 4 — **no kg pin move** | 05C |
| [05E](05e-the-kg-cutover.md) | 5 | The pin plus every kg adaptation in one atomic commit: the `lisp_position()` funnel, `format`'s gates, `numberp`, the prelude equality rewrite, the enumerated expectation allowlist, docs; closes the phase **and milestone 1**, triggering the milestone-2 re-pricing | 05D |

**05A is genuinely first, for the same structural reason 00A, 02A, 03A
and 04A were.**  Phase 5's semantics are ~40 independent oracle
questions, its representation is a union-and-enum question with an ABI
consequence, its equality family is a name-ownership question the
manifest checker enforces, and its complexity cost exceeds both fe
gates' headroom.  All of it must exist before any implementation slice.

**05B is mechanical and lands alone on purpose**, like 03B/04B: its
correctness argument is byte-identical goldens and a type no Lisp
program can produce.

**05C builds the tower against the host-API seam** — the 04C precedent
with a twist: instead of a transitional fallback that later dies, the
"fallback" is simply the unchanged reader, so there is nothing to
delete, only a lexer to replace.  Its tests drive integers in through
`FeMakeInteger` because no source text can spell one yet, and those
tests survive 05D unedited — that is their point.

**05D and 05E are the two halves of one Rule-10 delivery**, exactly as
02C/02D and 04D/04E were.  05D cuts fe over and closes the fe
workstream with the full nine-stage runner; it deliberately does not
move kg's pin — a pin-only commit cannot pass `lisp-compat-check` (the
new `eq` primitive collides with kg's prelude alias until 05E deletes
it) and every kg-side numeric expectation shifts.  05E is that pin,
with every kg adaptation in the same green commit.

## Handoff contract

Hand the slices to one engineer one row at a time; each sub-plan's
"Files this slice owns", test list and "does not do" section are part of
the acceptance criteria, not suggestions.  The five documents are removed
on completion, once the reviewer accepts the workstream; the Status
section then records what each slice actually delivered.

| Slice | Primary edit surface | Evidence it must add or preserve | Explicitly not its test |
|---|---|---|---|
| 05A | fe compat manifest/cases/snapshots, fe Makefile caps, Decision docs | version-stamped answers for the predicted case table, the union/enum spike, the collision table, funded caps proved live | no implementation, no status flips, no kg edits beyond the pin |
| 05B | fe `Value` union/type enum/constructors/writer arm/Fex GC arm, one new test, the fuzz reach-ahead | byte-identical goldens, no Lisp-observable change, `sizeof(Value)` asserted, forced-GC survival of an integer | no reader/printer change, no arithmetic, no version move |
| 05C | fe arithmetic/comparison resume arms, five new primitives, math-native return types, compat flips for the tower rows | `TestEvaluationControl`'s 4-step pin unedited, trace goldens byte-identical, host-API-driven tower tests, fuzzed overflow paths | no reader/printer change, no `eq`/`eql`, no version move, no kg source |
| 05D | fe reader lexer/printer/`eq`/`eql`/scripts/versions/fuzz dict | `reader-no-integers` flips supported un-regenerated, `mandelbrot` golden byte-identical via script migration, 05C's tower tests unedited, full nine-stage fe runner | no kg pin, no kg source, no compatibility residue |
| 05E | kg pin, the `lisp_position`/`format`/`numberp` funnel, prelude equality rewrite, enumerated expectations, manifests, docs | the expectation allowlist and nothing outside it, `lisp-compat-check`/`lisp-prelude-check` green, full parallel runner, the milestone-2 re-pricing | no new kg natives, no clamp-policy change, no oracle regeneration |

For all rows, current suite counts are a starting census, not literals to
assert.  Run focused tests while iterating; the full Fe runner closes the
Fe workstream at 05D, and kg's full parallel runner closes Phase 5 — and
milestone 1 — at 05E.

## Compatibility direction

The parent's §0.4 is binding for every sub-plan: there are no known external
users of this Fe fork and no known user-written kg `init.el` files.  Here,
"compatibility" means agreement with the pinned Emacs oracle, not preservation
of the old Fe/kg dialect.

Price and implement only the hard cutovers the two repositories need.  Do not
add legacy aliases, C-API wrappers, dual-evaluator modes, source-file lint for
hypothetical configs, or `.fe` filename fallbacks.  Version numbers still move
because they make the Fe↔kg contract checkable.

For Phase 5 this cuts one specific way.  There is no dual reader, no
old-printer mode, and no release in which `42` reads as a double against
a kg that expects integers — the reader/printer cut and the kg funnel
move as 05D + 05E, one Rule-10 pair.  Where fe cannot or should not
match Emacs, the divergence is *recorded*, not papered over: int64
instead of bignums (overflow is an `arith-error`-style message), `is`
stays fe's own broad comparator rather than pretending to be an Emacs
form, and out-of-range integer literals take the pre-bignum Emacs
behaviour with a divergence row saying so.  Version numbers move once,
at the cut, because they make the Fe↔kg contract checkable — kg's
`static_assert(FE_API_VERSION == 3)` firing at the 05E pin is the
designed tripwire, as it was in 03F and 04D.

## Sequencing

```text
05A  pin the target and fund ──> 05B  the integer object, dormant (pin moves)
                                      │
                                      v
                                 05C  the numeric tower, host-API seam (pin moves)
                                      │
                                      v
                                 05D  the cut, fe-only ── fe workstream closes, NO pin
                                      │
                                      v
                                 05E  the kg cutover ── pin + funnel + prelude + docs,
                                      one commit; closes the phase and milestone 1
```

Strictly linear, and nothing runs in parallel with it: every slice edits
either the numeric representation or the things that read it.  Every
arrow is a real dependency: 05B implements the representation 05A
measured, 05C dispatches on the type 05B landed, 05D feeds the tower
05C built, and 05E adapts kg to the contract 05D versioned.

The pin discipline is the Phase 4 shape verbatim: 05B and 05C move kg's
pin in their own trivial green commits (05C's carries the five new
primitive names through `lisp-compat-check` and the moved minimum-arena
figure), 05D moves nothing, and 05E is the pin.

## What this set deliberately does not do

- **No bignums.**  Integers are `int64_t`; arithmetic overflow and
  out-of-range literals are recorded divergences against Emacs' bignum
  behaviour, with `arith-error`-style messages, not silent wraps.
- **No condition system.**  `arith-error`, like `void-function` before
  it, is a name carried in the `FeHandleError()` message, not a
  signalable condition; no test or compat case may assert a catchable
  condition object.  Conditions are Phase 6, and this phase's overflow
  and zero-divide errors are their first customers.
- **No `number-to-string`/`string-to-number`, no `min`/`max`/`abs`/
  `mod`, no `?a` character literals, no `#x`/`#o`/`#b` radix syntax.**
  None are in the parent's Phase 5 list; the first package that needs
  one is Phase 8's evidence for adding it.
- **No clamp-policy changes in kg.**  `goto-char`'s clamping,
  `lisp_optional_count`'s saturation and `goto-line`'s bounds are
  documented Emacs-matching behaviour and keep their shapes with
  integer inputs.
- **No Fex changes beyond the GC arm.**  Fex's math natives
  (`fex_math.c`) keep their own semantics; the standalone `fe` binary's
  Fex shadowing of `floor`/`ceiling`/`log`/`round`/`truncate` is a
  recorded fact 05C works around in its tests, not a thing to fix.
- **No strict arity, no interactive arguments.**  Phase 7.  `/=`'s
  binary-only arity and the chained comparators' behaviour are pinned by
  oracle cases, but lambda arity stays lax.
- **No compatibility residue.**  No dual reader, no old-printer flag, no
  `is`-aliased `eq` reachable after the cut (§0.4).  Version numbers
  still move because they make the Fe↔kg contract checkable.
- **No re-litigation of recorded Phase 4 residue.**  `FeTMacro`,
  `internal--let`, and the message-level condition rule all stand as the
  Phase 4 Status recorded them.

## Rules

The follow-up program's rules (`../2026-07-31-follow-ups/README.md`) apply
unchanged, and three of them do most of the work here:

- **Rule 6** — no ratchet is raised for routine work; record scc before and
  after; bank a decrease when one lands.  The Decision below is where the
  non-routine exception is written down, and every phase from 2 onward
  adds its own dated Decision there rather than borrowing that one.
- **Rule 9** — each commit runs its focused suite; each phase ends with
  `make check` and `make WITH_LISP=0 clean all check`; a completed
  workstream ends with `.ci/run-ci-steps.sh --parallel`.
- **Rule 10** — Fe changes land and pass in the submodule first, then the
  parent pin and all kg adaptations move together in a separate, green kg
  commit.  A deliberate API/language break must not create a pin-only commit
  that cannot build.

Two additions specific to this program:

- **Run both complexity commands, in both trees.**  `make
  complexity-check` and `make pmccabe-check` measure different things;
  sub-plan 07B was caught by the second after passing the first.  Fe has
  its own pair, and a cap can sit silently red for a whole plan.  Run them
  at the *start* of a slice as well as the end.
- **Every phase touches `doc/fe-upstream.md`'s divergence table.**  That
  table currently documents, as deliberate decisions, most of what this
  program reverses.  A phase that changes a row and does not rewrite it
  leaves the authoritative document lying.  Phase 2 alone invalidates the
  rows for assignment `=` and one-namespace `setq`.

One more, learned the hard way during Phase 2 and binding on this set:

- **For anything below `fe.c:1010`, `pmccabe`'s summed complexity is the
  measurement and `scc`'s total is a floor.**  Phase 2 added two special
  forms, a primitive and a chained comparator, deleted an arm, and scc
  moved by exactly zero.  03A makes this structural by adding a pmccabe
  total **funded budget** — the measure a translation-unit split conserves
  — beside the existing per-symbol ratchet, and
  Phase 3's Decision records which gate is authoritative for the core from
  then on.  The audited starting total is **500 across 202 symbols**;
  `make pmccabe-baseline` may migrate per-symbol path keys but may not raise
  the Makefile's total budget.  Do not price, report or bank a Phase 3
  change in scc units without the pmccabe number beside it.

Two more, learned the hard way during the first set:

- **Measure from an idle tree.**  A slice reported 31 native suites and
  391 PTY cases where the tree has 32 and 405, because two `make`
  invocations were clobbering each other's objects.  The real numbers are
  **32 native / 405 PTY**, and **337 pass + 68 skip** under `WITH_LISP=0`.
  These are the 2026-08-04 starting baseline; later slices deliberately add
  tests, so compare the runner's discovered total with the prior slice
  rather than demanding these numbers forever.  An unexplained decrease or
  an early mismatch still means contention until proven otherwise.
- **A CI lane run against a stale `compile_commands.json` is not
  evidence.**  `ci-06` passed on a single-lane run that was analysing the
  pre-change translation unit, and then failed in the full parallel run on
  a missing `<stddef.h>`.  Regenerate the compile DB before believing that
  lane.

## Complexity price table (00A, 2026-08-04)

Every row below is priced against the closest existing module, per
00A's method.  Only two numbers in this table are *measured* rather than
estimated: the "Phase 0 (fe)" row, which is the raise this slice actually
landed, and the "split tax" component of Phase 3, which is the throwaway
spike's result (see 00A's own document for the spike).  Everything else is
an estimate from reading the parent plan's phase section against the
shape of the nearest existing code, in the spirit of the follow-up
program's Plan 06 table — a number to fund the next concretely-scoped
piece of work against, not a prediction to be silently missed.  Phases
6–10 are milestone 2 (parent §0.3) and are marked **provisional**: they
are priced here because the sub-plan asks for a row per phase, but they
are *not* funded by this slice's Decision, and should be re-priced when
milestone 1 closes and hands back real measurements.

Hard-cut design (parent §0.4) is priced throughout: no row below reserves
complexity for a compatibility wrapper, an `=`-assignment lint/deprecation
path, a second evaluator, `.fe` fallback loading, or a lax-arity mode.

### fe tree (`SCC_COMPLEXITY_MAX`, all sources; measured 2026-08-04 baseline 210/210, `fe.c` 102/105 file cap)

| Phase | What it builds | Priced against | Estimate | Status |
|---|---|---|---|---|
| 0 — freeze & baseline | Read-only arena counters: object/free slot counts, peak live objects, GC count, hooked into `MakeObject`/`CollectGarbage` | `GetDouble`/`GetNativeFn`-shaped trivial accessors, plus a few counter increments in existing functions | **+10** (measured raise, see Decision) | **Landed. Actual +4** (210 -> 214), so 6 of the 10 funded points are unspent |
| 1 — prelude extraction | fe untouched (kg-only phase) | — | 0 | **Landed. Actual 0**, as predicted |
| 2 — hard-cut `=`/`setq` | `setq` as a core special form (pair iteration over the existing assignment path), `set` as an ordinary-semantics primitive using the global setter, and a left-to-right chained double `=` arm | `EvaluatePrimitive`'s existing assignment arm (part of its 15 pmccabe today); `PLess`/`PLessEqual` as type-checking references, but not as an arity/iteration template | **+20 to +30** (net of deleting the old assignment arm it replaces) | **Landed. Actual +6, and scc could not see it.** No cap crossed, so no Decision. scc stayed at 214/220 through both slices; `pmccabe`'s `fe.c` sum went 340 -> 350 (02B) -> 356 (02C), which is the real number. Phase 2's code sits past `fe.c:1010`, where scc's parser desyncs -- see the Phase 2 Status below |
| 3 — frame machine | Explicit evaluator frame stack, 12+ frame kinds, resumable state, GC-stack/cleanup checkpoint migration into frames, `fe.c` split into ≥2 translation units (recommended, §0.2/§3 below) | Today's recursive core (`Evaluate`, `EvaluatePrimitive` 15, `ArgsToEnv` 13, `DoList`, `EvaluateList`, `EvaluateHead`, `GetBound` ≈ 35–45 pmccabe combined) roughly doubled, **plus** the measured +42 split tax | **+42 (measured split tax) + 60–100 (frame-machine substance) ≈ +100 to +140** | **Next — and the unit is wrong.** The +42 is 00A's *reader* extraction; Phase 3 extracts the *evaluator*, which sits entirely below `fe.c:1010` and is therefore invisible to scc today. Expect a larger jump from un-blinding a bigger region, and expect it to be a blind spot being paid off rather than new complexity. 03A re-prices this from a spike of the real cut and settles the measurement unit before 03B lands |
| 4 — Lisp-2 | Symbol value/function cell accessors, function-position lookup, `function`/`funcall`/`apply`/`fboundp`/`symbol-function`/`fset`/`fmakunbound`/`defalias`, `#'` reader change | ~10 small new functions at 3–6 pmccabe each | **+40 to +60** | **Landed inside the +60 funded raise** (04A: scc 420→480, file 240→300, pmccabe 630→690).  Phase actual: scc 391→441, pmccabe 601→643.  The post-close review fixes (see the Phase 4 Status addendum) spent a further +18 scc / +17 pmccabe of the same funding on defect repair: measured close is **459/480 and 660/690** |
| 5 — integers | `FeTInteger` in the existing `Value` union, an Emacs number lexer replacing `strtod`, shortest-round-trip float printing, either-type `ResumeArith`/chained comparators, `>`/`>=`/`/=`/`integerp`/`floatp`/`eq`/`eql`, per-function math-native return types | The tower arms extend `ResumeArith`/`ResumeBinary`/the `=` arm in place (the `ARITH_OP`/`NUM_CMP_OP` macros this row originally named died with 03E) | **+50 to +70** | **Next.  Exceeds both fe gates' measured headroom (23 scc, 32 pmccabe)** — 05A's Decision funds it before 05B lands, the 00A/03A/04A pattern |
| 6 — conditions | 5 completion kinds, condition hierarchy, `catch`/`throw`, `condition-case`, checkpointed cleanup | Comparable in shape to Phase 3's control-flow weight | **+70 to +100** | Provisional (milestone 2) |
| 7 — strict arity | Unconditional strict arity, arity checks per primitive/special form | Fe's `-a` pass already exists; mostly small per-site additions | **+20 to +30** | Provisional (milestone 2) |
| 8 — init compat waves | `let`/`let*`/`progn`/`prog1`/`cond`/`while`/`and`/`or`/`defvar`/`defconst`/keyword self-eval move from Lisp prelude into core; Wave C reader additions | ~8 new core special forms at 4–6 pmccabe each, plus reader work | **+50 to +70** | Provisional (milestone 2) |
| 9 — robustness | Arena-stat API extension, iterative/pointer-reversal GC marking (replaces recursive mark), resource-exhaustion coverage | `CollectGarbage`'s mark phase, rewritten | **+40 to +60** | Provisional (milestone 2) |
| 10 — proofs | `.fe`/`.el` proof packages and fixtures | Not C; not scc-scanned | 0 | n/a |

Milestone 1 (phases 0–5) sums to roughly **+270 to +330** fe points on top of
today's 210 — more than doubling the cap before milestone 1 closes. That is
the single most important number this table produces: **fe's cost is
structural, not incremental**, because Phases 3, 5 and (in milestone 2) 6
build subsystems fe.c has no ad hoc predecessor for, the same condition
that forced kg's own 2026-08-02 re-baselining decision in the follow-up
program.  Do not read the range above as a promise; it is what the "price
every phase" method produces today, and Phase 3 in particular carries the
widest uncertainty in the whole table.

### kg tree (`SCC_COMPLEXITY_MAX`, `src` only; measured 2026-08-04 baseline 5439/5500, 61 headroom)

| Phase | What it builds | Priced against | Estimate | Status |
|---|---|---|---|---|
| 0 — freeze & baseline | `test/lisp-compat/` manifest + checker | Python (`utils/`), not scc-scanned | 0 | n/a |
| 1 — prelude extraction | Deletes 4 `(list 'quote nil)` workarounds and 2 stale comment claims in `src/lisp_prelude.c`; generator is Python | `src/lisp_prelude.c` | **−3 to −5** (net decrease) | **Landed. Actual −1** (5444 → 5443).  The estimate also assumed the 4 `(list 'quote nil)` workarounds would go; 01A left them, since removing them changes evaluated code and Phase 2 rewrites all four forms anyway |
| 2 — hard-cut `=`/`setq` | Deletes a Lisp-source macro, renames `.fe`→`.el`, changes discovery string literals, and adds one compile-time language-version assertion; no new loader branch | existing `src/lisp_core.c`/`lisp_io.c`/`lisp_require.c` seams | **0 expected** | **Landed. Actual +1** (5443 -> 5444), the `static_assert` line; the estimate held. pmccabe unchanged at 1246 symbols, 0 new/gone/improved |
| 3 — frame machine | Adapts `src/lisp_core.c` call sites to the new Fe API version; no new kg-side control flow | `src/lisp_core.c` | **+10 to +15** | **Next, and likely an overestimate.** 03F's kg-side inventory is two `static_assert`s, a field rename through `lisp.h`/`perf.h`/`lisp_core.c`, a test expectation and a bench label. Eleven Makefile sites (03B) are not scc-scanned. Expect ~0 |
| 4 — Lisp-2 | Command registry's rooted-callable lookup moves from value cell to function cell; `defun`/`defmacro` rewrite is Lisp, not C | `src/lisp_cmd.c` (57) | **+15 to +25** | **Landed at ~0** — scc unchanged at 5444 at phase close; the post-close review fixes (designator-diagnostic seam, `functionp` via `FeIsFunction`) added **+6 → 5450/5500** |
| 5 — integers | The `lisp_position()`/`lisp_finite()` funnel becomes integer-typed, `format`'s two type gates widen, `numberp` goes two-tag, the prelude equality family is rewritten | `src/lisp_buffer.c`/`lisp_io.c`/`lisp_cmd.c` seams | **+15 to +25** | **Next, and likely an overestimate** — the funnel audit found 7 constructor sites + 2 gates + 1 predicate, not the parent's scattered ten; 50 points of measured headroom (5450/5500), no raise expected |
| 6 — conditions | Translates new host-visible completion categories at kg's error/signal boundary | `src/lisp_core.c` and callers | **+25 to +40** | Provisional (milestone 2) |
| 7 — strict arity | Interactive argument metadata, interactive-spec parser, argument construction | `src/lisp_cmd.c` (57), **priced by 00A's own anchor table as "roughly doubling"** | **+55 to +65** | Provisional (milestone 2) |
| 8 — init compat waves | `defcustom` validation + docstring storage (Wave D); no Customize UI | `src/lisp_prelude.c`/`src/lisp_require.c` | **+25 to +40** | Provisional (milestone 2) |
| 9 — robustness | Arena-stat diagnostics command | small, new | **+10 to +15** | Provisional (milestone 2) |
| 10 — proofs | Fixtures, PTY cases | Not `src/*.c` | 0 | n/a |

Milestone 1 (phases 0–5) sums to roughly **+35 to +65** kg points — comfortably
inside the existing 61-point headroom, and likely self-funding once Phase
1's deletions and Phase 2's macro removal land.  **kg is not the
constraint anywhere in this program; fe is, throughout.**

## Decision — fe complexity caps raised for Phase 0, no fixed end-state cap set

Taken 2026-08-04, closing sub-plan 00A.

**The raise, and what it is not.**  `fe/Makefile`'s `SCC_COMPLEXITY_MAX`
moves **210 → 220** and `SCC_FILE_COMPLEXITY_MAX` moves **105 → 112**,
landed in the fe submodule (commit `2a045bf`, branch `analyzers-etc`) with
kg's gitlink moved to match in a separate commit, per Rule 10.  This is a
**raise with a named, immediate funding target**: it exists to let sub-plan
00D land Phase 0's read-only arena counters (priced above at +10), which
kg's own Phase 0 baseline cannot be taken without (parent §0's "ordering
correction").  It is sized for that one piece of concretely-scoped work
plus a small margin, not for the program.

This is deliberately **not** the "raise with named repayment sources" shape
the follow-ups README uses elsewhere (Plan 03's 200-unit loan against
`search.c`/`cmd.c`/`localvars.c`): fe has no ad hoc predecessor code to
delete in exchange, for the same reason the price table above shows every
remaining phase as net-additive.  It is also not a "raise without funding"
in the sense of covering unscoped future work — it funds exactly the next
named deliverable (00D) and nothing past it.

**No fixed end-state cap.**  The price table above sums milestone 1 to
roughly +270 to +330 fe points — a number more than doubling today's cap —
but this Decision does **not** set that as fe's future
`SCC_COMPLEXITY_MAX`.  The follow-up program's 2026-08-02 Decision withdrew
exactly this kind of promise (the 4223 ceiling) after two estimates were
missed low and one cap sat silently red for a whole plan; repeating the
mistake here, on a program whose central risk (the frame machine, Phase 3)
carries the widest uncertainty this table has, would be worse, not better.
**Every phase from Phase 2 onward gets its own dated Decision, in its own
sub-plan, using this table's row as the starting estimate and this
slice's measured 210/220 as the tree's actual state — re-measured, not
assumed, per Rule 6.**  kg's caps are untouched: 5439/5500 stands, and the
price table shows kg's milestone-1 total fitting inside existing headroom
without a raise.

**The translation-unit decision.**  Recommendation (b) from 00A's own
document is confirmed: **split `fe.c` into more than one translation
unit, decided now, executed in Phase 3**, with (a) — a bounded file-cap
raise, exactly what this Decision does — as the interim fallback for
commits before Phase 3.  The cost inventory in 00A's document (Makefile
sites, `lisp-include-check`, fe's own `SOURCES`/`SCC_COMPLEXITY_PATHS`,
`doc/fe-upstream.md`'s embedding paragraph, `WITH_LISP=0`) was verified
site-by-site against this tree, by symbol rather than line number, and
every site named there exists as described.

**What the spike adds to that recommendation, and changes about its
price.**  The throwaway spike (00A's document, "Spike") split the reader
(`Read`, `ReadList`, `ReadAtom`, `ReadWrapped`, the rparen sentinel, plus
`FeRead` itself and the private object/context layout the reader needs)
into `fe_reader.c` behind a new `fe_internal.h`.  `make -C fe check` and
kg's `make check` were both green.  `pmccabe`'s per-function sum was
**exactly conserved across the split** (335 before, 335 after) — the
authoritative, whole-file measure the fe Makefile's own comment already
names as trustworthy. But **scc's total was not conserved**: it rose from
210 to **252**, a +42 increase from a purely mechanical, behavior-free
move. `fe.c`'s own file score barely moved (102 → 100); `fe_reader.c`
alone scored 44. The fe Makefile's existing comment already flags why:
scc's C parser desynchronizes on `fe.c`'s `'"'` character literals and
undercounts keywords following the desync, so scc's total was never a
true measurement of `fe.c`, only a floor. Extracting a quote-heavy region
out of the file that was hiding it changes how much of the surrounding
code the scanner sees. **The practical consequence for Phase 3's own
Decision: budgeting the split as "moves file-cap points into a second
file, net-neutral on the total" is wrong. A real split should expect scc's
total to jump by tens of points from the split alone, before a single
line of frame-machine logic is added**, and Phase 3's own price row above
already includes that measured +42, not a placeholder.

## Decision — Phase 3 complexity caps and the frame-storage answer (03A, 2026-08-04)

Taken 2026-08-04, closing sub-plan 03A.  Re-measured against the tree as it
stood after Phase 2 closed, per Rule 6: kg **5444/5500** unaffected (this
slice touches no `src/*.c`), fe **214/220** with `fe.c` at **106/112**,
pmccabe **500 across 202 symbols**, `fe.c` alone **356 across 135 symbols**,
worst function `EvaluatePrimitive` at **15**.

### The evaluator split, measured for real

00A's spike extracted the *reader*; Phase 3 needs the *evaluator* out, and
03A's own document is explicit that the two are different measurements
because the evaluator sits entirely past `fe.c`'s `'"'`-literal scc desync
(line 1010) while the reader straddles it.  A throwaway branch
(`03a-spike-eval-split`, off `analyzers-etc` at `5cc0acb`, deleted after
measuring) executed exactly 03B's cut: `ClearEvaluationControl`,
`BeginEvaluationControl`/`EndEvaluationControl`, `EvaluationStep`,
`EnterEvaluationDepth`, the cleanup registry (`PushCleanup`,
`SaveCleanupErrorMessage`, `RunOneCleanupEntry`, `RunCleanupsDownTo`,
`RunCleanupsAfterError`, `FeProtectWithCleanup`), `FeHandleError` (moved
with the cleanup registry per 03B's own recommendation), the evaluator
proper (`Evaluate`, `EvaluateHead`, `EvaluatePrimitive`, `EvaluateList`,
`DoList`, `Bind`, `ArgsToEnv`, `EvaluateSetq`, `EvaluateSet`,
`EvaluateNumericEqual`, `CheckNumericEqualOperand`, `HandleVoidSymbol`,
`HandleNonCallable`, the `EVAL_ARG`/`ARITH_OP`/`NUM_CMP_OP` macros), and the
evaluator entry points (`FeEvaluate`, `FeCall`, `FeCallWithOptions`,
`FeEvaluateWithOptions`) into a new `fe_eval.c` behind a private
`fe_internal.h`, leaving the reader, writer, `FeGetRoot`/`FeCreateRoot`/
`FeReleaseRoot`, and the kept wrappers (`EvaluateInput`,
`FeEvaluateString*`, `FeEvaluateFile*`) in `fe.c`, exactly as 03B's document
specifies.  `unbound` lost `static` (its address is compared across both
files); the accessors 03B's document names (`GetDouble`, `GetNativeFn`,
`SetType`, `CheckType`, `GetBound`, `MakeObject`, `Equal`, `IsNamedSymbol`,
`Format`) lost `static` and gained declarations in `fe_internal.h` instead.

| Measure | Before spike | After spike (evaluator extracted) |
|---|---|---|
| `pmccabe` total (`PMCCABE_PATHS`) | 500 / 202 symbols | **500 / 202 symbols — exactly conserved** |
| `pmccabe` `fe.c` | 356 / 135 symbols | 252 / 106 symbols |
| `pmccabe` `fe_eval.c` | — | 104 / 29 symbols |
| `scc` total (`SCC_COMPLEXITY_PATHS`) | 214 | **286 (+72)** |
| `scc` `fe.c` | 106 | 70 |
| `scc` `fe_eval.c` | — | **108** |

**pmccabe is conserved exactly, again** — 500 before, 500 after, split
106+29=135 fewer/29 more symbols between the two files but the same 356
points that were `fe.c`'s alone before, now split 252 (`fe.c`) + 104
(`fe_eval.c`).  This is the second data point (after 00A's reader spike)
for "pmccabe is conserved across a translation-unit split; scc is not."

**scc's total jumped +72 — more than 00A's +42, as 03A's document
predicted.**  The evaluator was almost entirely invisible to scc before
(the whole 1443-line evaluator region contributed only part of `fe.c`'s 106
points, most of which is reader/writer code above the desync); once it is
its own file with no `'"'` desync ahead of it, scc counts it honestly:
`fe_eval.c` alone scores 108.  `fe.c`'s own score fell to 70 (some of its
106 was evaluator-adjacent glue that left with the move).  **`fe_eval.c`
scores 108 of the *old* 112 file cap before a single frame-machine line
exists** — four points of headroom for the phase priced in the table above
at +60 to +100 substance.  This is the numeric confirmation of the price
table's warning: the file cap, not the total, is what forces the split,
and the split alone nearly exhausts it.

`make -C fe check` and kg's `make check` (32 native / 406 PTY, all passing
— 02E's independent slice had already added one PTY case to the idle-tree
baseline by the time this measurement ran) were both green on the spike,
confirmed with a temporary, uncommitted two-line change to kg's `Makefile`
(`FE_OBJ` as a two-object list, `$(OBJDIR)/fe_eval.o`'s build rule) that was
reverted before the branch was deleted.  Both `fe.c`/`fe_eval.c` compiled
clean under `-Weverything -Werror` in both translation units.

### The complexity caps this raises

Both re-measured totals above (`500/202`, `214/220`) are the *pre-spike*,
currently-landed state — nothing in the split or the frame machine has
landed for real, so both caps below are raised **ahead of the work they
fund**, exactly as 00A's own Decision raised 210/105 → 220/112 ahead of
sub-plan 00D landing.

- **`PMCCABE_TOTAL_MAX`: 500 → 630.**  03B's split costs this budget
  nothing (conserved exactly, measured above).  03C–03E's substance is
  estimated by roughly doubling `fe_eval.c`'s own *measured* evaluator
  weight — 104 points across 29 symbols, the real number, not the stale
  35–45 cross-file estimate the price table used before this slice — for
  +100 to +140, landing the total at 600–640.  Funded at **630**, near the
  top of that range with a small margin, not the program's full
  uncertainty range.  This is now **the authoritative aggregate measure
  for the core** (`fe.c` + `fe_eval.c`); the per-symbol
  `.ci/pmccabe-baseline.json` manifest and scc's total/file ratchets are
  retained as secondary checks (an unbudgeted new file, or complexity in
  the `fex_*` files where no desync applies).  `utils/check_pmccabe_complexity.py`
  now takes a required `--max-total`, sums the parsed functions, and fails
  before `--write-baseline` can run if either the per-function or the
  total budget is exceeded — `make pmccabe-baseline` cannot launder a tree
  that is over either budget into a clean-looking manifest.  Proved by
  temporarily setting `PMCCABE_TOTAL_MAX=499` (one below the measured
  500): both `make pmccabe-check` and `make pmccabe-baseline` failed with
  `FAIL: total complexity 500 exceeds funded budget 499 (+1)`, the
  baseline file was left untouched by the refused `pmccabe-baseline` run,
  and both commands passed again once the override was removed.  The
  per-symbol baseline was also re-recorded in this slice (`make
  pmccabe-baseline`, 198 → 202 symbols), banking the four Phase-2 symbols
  (`EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`,
  `CheckNumericEqualOperand`) that had drifted un-ratcheted since 02B/02C.
- **`SCC_COMPLEXITY_MAX`: 220 → 420.  `SCC_FILE_COMPLEXITY_MAX`: 112 → 240.**
  The total funds the measured split cost (214 → 286, +72) plus the same
  +100 to +140 substance estimate used for pmccabe's total (scc and
  pmccabe move in the same rough proportion here because both counts came
  from the same spike).  The file cap funds `fe_eval.c` specifically:
  108 today, +100 to +140 substance → 208–248, funded at 240.  Both are a
  single shared constant across every file scc scans (unchanged
  infrastructure); the practical effect is that `fex_*.c` files also gain
  headroom they do not need, which is an accepted, deliberate trade for
  not building a second complexity-checking mechanism this program does
  not otherwise need.  scc's total and file caps remain in force — not
  deleted — because they still catch what pmccabe cannot: an entirely new
  unbudgeted file, or runaway complexity in `fex_re.c`/`fex_io.c`/
  `fex_process.c`, none of which sit past any desync.

### The C-stack probe: what "before" looks like

`TestEvaluationStackProbe` (`fe/test_api.c`) registers a `stack-probe`
native that records `(uintptr_t)__builtin_frame_address(0)`, driven by
`(defun deep (n) (if (<= n 0) (stack-probe) (+ 1 (deep (- n 1)))))`.  It
measures the same native against itself — a bare `(stack-probe)` call as
the depth-zero reference, then `(deep n)` for n = 10, 100, 200 — and prints
the address delta.  It also proves the existing depth-limit error recovers
cleanly (a tight explicit `max_depth`, matching `TestEvaluationDepth`'s own
pattern) without poisoning the context, and asserts `(deep n)` evaluates to
`n` at each depth, so the probe cannot be silently running at the wrong
semantic point.  It is a recording test, not a flatness gate — 03F adds
that assertion and the dynamically sized 100000-depth run.

| Build | n=10 Δbytes | n=100 Δbytes | n=200 Δbytes | ≈bytes/level |
|---|---|---|---|---|
| default (`clang`, no `-O`, per fe's own `CFLAGS`) | 24288 | 231648 | 462048 | ~2305 |
| ASan/UBSan (`ci-04` flags, `-O1 -fno-omit-frame-pointer`) | 12576 | 119136 | 237536 | ~1185 |
| MSan (`ci-05` flags, `-O1 -fno-omit-frame-pointer`) | 19376 | 183536 | 365936 | ~1825 |

All three grow linearly in `n`, confirming the property the Phase 3 gate
names (a measured C-stack high-water mark that grows with Lisp nesting)
exists to measure and will need to stop being true.

Manually swept (not part of the automated test, to keep `make check`
deterministic and fast on every build) with `./fe -s <arena-size>` and a
bounded search that never approaches an uncontrolled host-stack crash,
because sub-plan 06E's `evaluation_depth` counter already intercepts every
build well before either the GC-stack (4096 slots) or the real C stack is
threatened:

- With fe's default 64 KiB arena (`main.c`'s default and `test_api.c`'s
  `TestArenaSize`): `(deep 294)` succeeds, `(deep 295)` raises `out of
  memory` — object-slot exhaustion, identical across default, ASan/UBSan
  and MSan builds.
- With an arena of 128 KiB or larger (object exhaustion no longer the
  binding constraint): `(deep 332)` succeeds, `(deep 333)` raises
  `evaluation depth limit exceeded` — `DefaultEvaluationDepth`'s 1000-unit
  ceiling — identical across all three builds, with **no host-stack crash
  in any of them**.
- `(deep 100000)` does not run in any build or arena size tried, and was
  not pushed toward one: reaching it would need both `max_depth` raised far
  past 1000 and an arena sized in the tens of megabytes, which is exactly
  the "drive a sanitizer process into a host-stack crash" 03A's document
  warns against chasing by hand.  §4 below computes what it actually needs.

**What 03A's own document got wrong here.**  Its own "why it cannot be
deferred" section cites two anecdotes — GC-stack overflow past `(deep
452)`, an MSan crash at `(deep 418)` — as the closest existing evidence.
Both are *pre-06E* facts, from before `evaluation_depth` existed.  Neither
reproduces on this tree: 06E's counter already caught up with them, and
today all three builds hit the *same* clean, catchable boundary (332/333)
regardless of sanitizer, well inside both the old GC-stack accident (4096
slots) and any real C-stack limit.  That is good news — kg's users are not
sitting on a live crash bug — but it means the "before" picture is not the
crash-adjacent one the document expected; it is a flat, safe wall at
n=332/333 today, and Phase 3's job is to move that wall out to 100000
without changing what happens below it.

### Where frames live, and how many fit

Measured (not guessed) on a throwaway draft `FeEvalFrame`, compiled
standalone against `fe_internal.h`'s real object layout — kept off the
split-measurement branch per 03A's document ("must not be mixed into the
split measurements"), on its own disposable file, never wired into
`fe.c`/`fe_eval.c`:

```c
typedef struct FeEvalFrame {
  FeFrameKind kind;          // 14 draft kinds: call/args/dispatch, lambda
                              // and macro body, macro expansion, if/and/or/
                              // while/let/setq, unwind-protect, native re-entry
  FeObject* expr;             // the form this frame is evaluating
  FeObject* env;               // lexical environment
  FeObject* rest;               // remaining args/body forms
  FeObject* accumulator;         // partial result / builder
  FeObject* callee;               // resolved call head, when applicable
  size_t gc_checkpoint;             // FeSaveGC() snapshot to restore to
  size_t cleanup_checkpoint;         // cleanup_stack_index snapshot
  FeObject trace_cell;                // embedded call-trace cell (below)
} FeEvalFrame;
```

`sizeof(FeEvalFrame) = 80`, `alignof = 8` — compiler-measured, `clang
-std=c2x`, same object layout `fe_internal.h` gives both real translation
units.  80 and `sizeof(FeObject)` (16) are both multiples of their shared
alignment (8), so a frame region followed immediately by an object region
needs no inter-region padding.

**Peak frames per `(deep n)` level: 3, derived and cross-validated, not
guessed.**  Walking the grammar's live-nesting structure — which pair-form
`Evaluate()` calls are still *open*, waiting on a nested one, at the
instant `n` reaches 0 — for one level of `(if (<= n 0) BASE (+ 1 (deep (-
n 1))))`, exactly three stay open simultaneously: the call to `deep`
itself, the `if`, and the `+` waiting on its second argument.  (`(<= n 0)`
and `(- n 1)` both close before the next nested pair-form opens, so they do
not add to the *simultaneous* peak, only to the total evaluation count.)
That gives `peak_depth(N) = 3N + c` for a small constant `c`.  This is
independently cross-checked against §1's own empirical boundary: with
`DefaultEvaluationDepth = 1000`, `evaluation_depth` is *exactly* this same
live-nesting count today (it increments on every pair-form `Evaluate()`
entry and decrements on return), so `peak_depth(N) = 3N + 2` predicts
failure starting at `N = 333` (3·333+2 = 1001 > 1000) and success through
`N = 332` (3·332+2 = 998 ≤ 1000) — which is exactly the measured 332/333
boundary in §1.  The derivation and the measurement agree to the unit.

**Partition formula (03A recommendation (a), superseded in 03C).**  `FeContext` gains a
pointer/capacity/index into a frame region carved from the same
host-provided arena (never a compile-time array member, and never a
separate allocation):

```
FeMinimumArenaSize() = sizeof(FeContext)                  // ~39464 (was 39440;
                                                            // +24 for a frame
                                                            // pointer + 2
                                                            // size_t fields)
                      + GetCoreObjectCount() * sizeof(FeObject)   // 256 * 16 = 4096, unchanged
                      + MinFrameCapacity * sizeof(FeEvalFrame)    // 64 * 80 = 5120
                      = 48680 bytes   (was 43536; +5144, +11.8%)

remainder = size - FeMinimumArenaSize()
frame_bonus_bytes = floor(remainder * 0.20)          // 20% of everything above the floor
frame_capacity = MinFrameCapacity + frame_bonus_bytes / sizeof(FeEvalFrame)
object_slots = 256 + (remainder - frame_bonus_bytes) / sizeof(FeObject)
```

`MinFrameCapacity = 64` supports `peak_depth` through `N ≈ 20` in the
*smallest legal* arena — comfortably above what fe's own `max_depth`-tight
test fixtures use (5, per `TestEvaluationDepth`'s pattern, reused by the
new probe test). The 20% split was a starting point, not a derived optimum.
**03C's landed implementation retuned it to 8%** after accounting for the
pending-error buffer and its object-headroom test; it also raised Fe's fixed
API and standalone arenas to 1 MiB. The formula and table below remain this
03A decision's provisional price model; 03C's Status records the
implemented configuration (document removed on completion — see git
history).

**03D measured actuals and Decision revisit (2026-08-04, retune follow-up).**
With the call frames landed, the draft's `sizeof(FeEvalFrame) = 80` is now 96
bytes — the `bind`/`fn` fields the resumable call frames added — and the
implemented arena numbers at the retuned 10% split are:
`FeMinimumArenaSize()` **53832** bytes; **1100 frames / 56210 object slots**
in kg's 1 MiB arena; **72 frames / 716 object slots** in the macro/GC test
arena (`FeMinimumArenaSize() + 8 KiB`, 62024 bytes), with the
self-expanding-macro exhaustion peak at 72 live frames (the 73rd push
refused).

**The partition was retuned from 8% to 10% after the first landing broke the
legacy limit.**  03A's Decision §4 "estimate too small" case came true twice
in a row.  First, 03D's landed frame is 96 bytes against the draft's 80, so
the 8% split that 03C chose gave kg's 1 MiB arena only **892 frames**; at ≈3
frames per `deep` level the canonical chain — `(defun deep (n) (if (<= n 0) 0
(+ 1 (deep (- n 1)))))` — hit the *physical* frame wall at **296/297** before
the logical `DefaultEvaluationDepth` 1000 ceiling (peak 3N+2 → the 332/333
wall) could fire.  The 8% split no longer preserved the legacy limit, so the
acceptance of that physical-first behaviour is withdrawn: `FrameArenaPercent`
moved 8% -> 10% (the minimal retune that clears the wall), kg's 1 MiB arena
now holds **1100 frames**, and the logical limit is binding again — the chain
succeeds at **332** and fails at **333**, exactly the 03A/03C numbers.  kg's
`test_perf` deep-call-chain fixture (`(deep 300)` returns 300) and
`test_recursion_depth` (`(deep 200)` returns 200, `(deep 5000)` raises, then
`(deep 200)` works again) are both green against the retune.  The physical
push check still raises the same transitional `evaluation depth limit
exceeded` text; no public API, option, error string or statistic moves
mid-migration, exactly as 03D's "no public bound changes" requires.  The
object-slot cost is stated honestly: the frame share grows from 8% to 10% of
the bytes beyond the minimum, so the 1 MiB arena's object capacity drops from
the actual 03C baseline of 57542 to **56210** slots (−1332, ≈2.3% of capacity)
— roughly half of the ~2590 objects kg's prelude itself keeps live at startup,
against a still-ample free margin (53078 slots free at the end of kg's
representative init).  **03E/03F are the place to
re-evaluate the frame layout and the two bounds**: 03E deletes the temporary
recursive paths that consume part of the ≈3 frames per `deep` level, and 03F
publishes the real frame and native re-entry bounds and must re-measure the
peak against the final frame layout before fixing any size.

**The `(deep 100000)` fixture size is no longer 128 MiB.**  03A's §3
estimate solved the 20%-split formula at the draft 80-byte frame.  At the
retuned 10% split and 96-byte frame the same derivation is **≈275 MiB**
(300002 frames × 96 bytes = 28,800,192 bytes (27.5 MiB) of frame region;
at 10% of the remainder that is 287,994,312 bytes (274.65 MiB) of arena).
Treat that as **provisional**: it is a rough re-derivation of the historical
§3 arithmetic, not a measured number, and it will move again when 03E/03F
change the frame layout and the per-`deep` frame count.  **03F must remeasure
the real peak before sizing its dynamically allocated fixture.**  kg's arena
is still untouched by this number — `(deep 100000)` remains an fe-side,
throwaway test allocation.

The 03A draft rows for the 20% split (2563/106 frames at 1 MiB/64 KiB) are
superseded by these measured numbers; the provisional formula and the
historical 128 MiB §3 paragraph above are retained as the 03A Decision, not
rewritten.

| Arena | Size | Frame capacity | `peak_depth` reach | Object slots | vs. today |
|---|---|---|---|---|---|
| fe `TestArenaSize` | 64 KiB (65536 B) | 106 frames | N ≈ 34 | 1099 | 1631 today, **−32.6%** |
| kg `KG_LISP_ARENA_SIZE` | 1 MiB (1048576 B) | 2563 frames | N ≈ 853 | 50254 | 63071 today, **−20.3%** |

1. **kg keeps its exact observable behaviour.**  2563 physical frames is
   2.56× `DefaultEvaluationDepth`'s 1000 (a 156% margin), so the existing
   *logical* 1000-unit ceiling fires first, unchanged — `test_recursion_depth`
   (`(deep 5000)` raises, `(deep 200)` still works) stays green **unedited**,
   because both depths sit far inside physical capacity (`peak_depth(200) =
   602`, `peak_depth(5000) = 15002`, both `< 2563`, and the logical check at
   1000 catches the 5000 case first exactly as it does today).

   **And the object-slot cut is measured against kg, not assumed.**  The
   −20.3% above is the one number in this Decision that could bite kg
   silently, so it was checked against 00D's own counters rather than
   argued: `utils/bench.py --case lisp-arena-representative-init` on the
   counting build reports `lisp_arena_peak_live` **3132** of
   `lisp_arena_total_slots` **63071** (5.0%), `lisp_gc_count` 0 and
   `lisp_alloc_failures` 0 for kg's representative init corpus.  Against
   the post-partition 50254 slots that is **6.2%** — a 16× margin, and the
   same run's `lisp_peak_eval_depth` of **53** sits at 2% of the 2563-frame
   capacity.  kg's real workload is nowhere near either ceiling; the
   partition costs it headroom it demonstrably does not use.  This is the
   arena-margin finding the set README's "no arena-size change in kg" rule
   asks for, and it says the 1 MiB arena needs no change.
2. **fe's 64 KiB fixtures still open**, with real but survivable headroom
   loss (1099 vs. 1631 object slots).  No existing `test_api.c` case is
   known to need more than that inside `TestArenaSize` — every depth-facing
   test already uses an explicit tight `max_depth` — but 03C owns
   re-verifying this once the partition is real, per its own gates
   (`TestContextCreation`'s exact-size boundary loop, `TestArenaStats`'s
   exact-fit case).  If either regresses, retune the fraction/floor before
   loosening a test.
3. **The gate's `(deep 100000)` is fe-side only, and here is what it
   actually costs.**  `peak_depth(100000) = 300002` frames = 24,000,160
   bytes (22.9 MiB) of frame region alone — not `100000 * sizeof(frame)`
   (7.7 MiB), which would undercount by the derived ×3 multiplier.  Solving
   the same 20%-split formula backward for a dynamically `malloc`'d arena
   that gives *at least* 300002 frames: **≈114.5 MiB**, rounded up to a
   **128 MiB** fixture for 03F's eventual dynamically-allocated `test_api.c`
   arena.  Large, but a one-time throwaway test allocation, not a
   constraint on kg or on fe's default footprint.  **kg's arena is
   untouched by this number** — nothing about `(deep 100000)` is a kg-side
   requirement (point 4 below).
4. **Restated gate wording** (03A's document asks for this explicitly): the
   parent plan's flatness property — *"a measured C-stack high-water mark
   is flat across `(deep 10)`, `(deep 1000)`, and `(deep 100000)`"* — is an
   **fe-side measurement**, taken by `test_api.c`'s dynamically sized
   arena.  kg is not expected to run `(deep 100000)`, and `./fe -s
   <large>` cannot either (the standalone interpreter never registers the
   test-only `stack-probe` native).  What kg's own tests assert is the
   weaker, host-relevant property: deep Lisp nesting is bounded by a
   **configured frame limit** that raises a deterministic, catchable error
   — never the C stack, and never arena exhaustion either, since the
   logical ceiling is sized to fire first (point 1).
5. **`max_frames` option**: `effective_frame_limit = (max_frames == 0) ?
   physical_frame_capacity : min(max_frames, physical_frame_capacity)` —
   zero selects the arena-derived physical capacity above; a nonzero value
   is an additional, caller-chosen ceiling never allowed to exceed it.
6. **Cleanup reserve**: physical frame storage is `frame_capacity +
   CleanupFrameReserve` (**32** frames, chosen the way `CleanupStackSize`
   (256) was — generously, not slot-matched to anything).  Ordinary pushes
   are checked against `frame_capacity`; once that ceiling is hit and an
   error is raised, cleanup-triggered pushes (running `unwind-protect`
   forms during unwind) may use the reserve, so a cleanup does not itself
   fail from frame exhaustion immediately after the body did.

**Call-trace storage: the embedded cell (adopted).**  `FeEvalFrame` above
already carries a plain, non-pointer `FeObject trace_cell` — the same
`CAR`/`CDR` shape as today's stack-allocated `FeObject cl`, but living in
context-owned frame storage instead of a C automatic.  `FeHandleError`
materializes the chain the host callback receives by walking live
*semantic* frames (excluding whichever kinds later turn out to be purely
internal resumption bookkeeping — 03C's own document already requires
this) and linking their `trace_cell`s, writing only into cells that already
exist: no arena allocation on the error path, satisfying `fe.h`'s existing
`FeErrorFn(..., FeObject *call_trace)` ABI unchanged.  This was accepted
per 03C's own condition ("accept only if the cleanup-at-capacity case is
demonstrated") because the arithmetic above already prices it in: the
32-frame cleanup reserve exists precisely so a cleanup that runs after
frame exhaustion can still push its own frames (and populate their own
trace cells) without touching — and so without corrupting — the original
error's already-linked chain, which references only frames at or below the
index captured when the error fired. 03C implements the exact snapshot
timing; this Decision fixes the representation and the reason a separate
parallel trace region was not needed.

**What this changes about the price table's Phase 3 row.**  The "+42
(measured split tax)" placeholder is superseded by the real number: +72
scc (measured) with pmccabe conserved at 0.  The "+60 to +100" substance
estimate is superseded by the pmccabe-anchored +100 to +140 above, applied
to both scc and pmccabe as this Decision's actual caps.  Total funded
Phase 3 scc cost: 220 → 420 (+200); pmccabe: 500 → 630 (+130, all of it
unspent substance headroom, since 03B's own cost is zero).

**03E measured actuals and Decision revisit (2026-08-05, final frame-layout
follow-up).**  With every special form and primitive converted and the
temporary recursive dispatch deleted, `FeEvalFrame` stays **96 bytes** — no
new struct field, since every new frame kind's state reuses the existing
`fn`/`rest`/`accumulator`/`callee`/`bind` fields via the same
`&unbound`-sentinel convention 03C/03D's call frames already established —
so `FeMinimumArenaSize()` (**53832** bytes) and kg's 1 MiB arena's **1100
frames / 56210 object slots** are all unchanged from 03D's numbers. This
Decision's own §1 ("kg keeps its exact observable behaviour") required a
real fix to hold, not just a measurement: 03E's first landing of the
`if`-false-branch conversion pushed every false branch through a generic
implicit-body wrapper, retaining a **fourth** simultaneously-open frame per
`(deep N)` level (the call, `if`, the implicit body, the arithmetic form
waiting on its second operand) against the **three** this Decision's whole
arena-sizing arithmetic assumes. 1100 physical frames ÷ 4 ≈ 275, so the
*physical* wall bound before the *logical* one (333) — not a theoretical
risk, but an actual regression in kg's own `test/test_perf.c` `(dc 300)`
fixture, caught by running kg's suite against the in-progress fe tree.
Fixed by special-casing the single-else-form shape (the common one, and the
one the canonical chain uses) to push that form directly instead of through
the wrapper, restoring exactly three frames per level and the exact
`(deep 332)`/`(deep 333)` boundary — the margin this Decision's §1 promised
is preserved, not merely re-measured after moving. Full derivation and the
fix itself are in 03E's own Status section
(`03e-special-form-frames-and-unwind.md`, removed on completion — see git
history).

**The `(deep 100000)` gate is not reached, and the reason is a wall this
Decision never modeled: `GcStackSize`.**  §3 above prices the *frame*
region's cost for `peak_depth(100000) = 300002` frames at ≈275 MiB and
calls it "the gate's actual cost" — but `GcStackSize` (4096), a fixed array
field of `FeContext` that is never carved from the arena, binds first once
`max_depth` is raised far enough to matter: the lambda-body wrapper alone
retains two GC-stack entries per still-open level (the `if` wrapper's own
entries are gone with the fix above, for the common single-form case), and
bisection against a standalone fe reproducer finds `(deep 1021)` succeeds
while `(deep 1022)` raises "GC stack overflow" — three orders of magnitude
short of 100000, and unrelated to arena size at all. This was never visible
before 03E because no earlier slice raised `max_depth` far enough past its
default (1000) to reach it; the canonical chain's logical ceiling at 333
sits comfortably below 1021 under the default configuration, so nothing
about kg's or fe's existing default-configured behaviour moved. 03E records
this as a finding for 03F rather than fixing it (`GcStackSize` resizing and
the arena question are both explicitly out of this set's "no arena-size
change" scope); the ≈275 MiB fixture-sizing estimate above is otherwise
unchanged (the frame layout that estimate depends on did not change) and
remains what 03F needs once `GcStackSize`'s own resolution unblocks it.

**Flatness, the property the parent plan's gate actually names, is real** —
measured at the largest N this tree can currently run end to end (N = 340,
comfortably clear of both the legacy 332/333 boundary and the GC-stack
bound's margin): the C-stack high-water mark is flat, delta 80 bytes,
against tens of kilobytes of growth over a comparable depth before Phase 3.
`(deep 100000)` at the literal value is what remains blocked, by the
`GcStackSize` wall above, not by anything this Decision's arena-sizing
arithmetic got wrong.

### kg

**Unchanged.**  kg is at 5444/5500 (61 points shy of the follow-up
program's own headroom accounting, unaffected since this slice adds no
`.c`/`.h` file to `src/`); the price table's own Phase 3 kg estimate
(+10 to +15) stands, funded from existing headroom, no raise needed.  The
only kg-side change in this slice is the pin move itself, in its own
green commit per Rule 10.

## Decision — Phase 4's Lisp-2 target, measured and funded (04A, 2026-08-05)

Taken 2026-08-05, closing sub-plan 04A.  Re-measured against the tree as
it stands after Phase 3 closed (fe `60e4c9e`, kg `3e3c946`), per Rule 6:
fe scc **391/420** total with `fe_eval.c` at **208/240** file cap; pmccabe
**601/630** across **230 symbols**, worst function **14** (`Read`,
`RunEvaluationLoop`); kg **5444/5500** (56 points of headroom).  Both fe
gates have exactly **29 points** of headroom against a phase priced
**+40 to +60** — the funding Decision comes first, again.

### The corpus: every prediction held

The seventeen predicted cases and their version-stamped Emacs 31.0.90
(`... of 2026-07-09`) snapshots are landed in `fe/compat/` as `planned`
entries (`source_name: null` until the primitives exist), and `make -C fe
compat` is green: **86 case(s), 54 passed, 32 known gap(s), 0 failed**,
the planned `lisp2-*` entries replaying as gaps against the current
one-namespace fe exactly as designed.  **No prediction was corrected by
the oracle** — every answer matched the predicted table, including the
headline `lisp2-value-function-coexist` → `(7 9)`, `lisp2-apply-spread` →
`10`, `lisp2-fboundp-primitive` → `t`, `lisp2-funcall-designator` → `9`,
the two `void-function`/`void-variable` messages, and
`lisp2-defalias-return-value` → `first`.  The three §1 data questions are
settled by that data: the roundtrip cases compare *behaviour*, not printed
closures; Emacs prints `(function car)` as `#'car` (snapshot confirmed),
leaving the writer-abbreviation question recorded for 04D; and
`lisp2-macro-representation` is checked in as `kg-policy`, pinning
`FeTMacro` so 04C does not imitate the `(macro . FUNCTION)` cons.  The two
existing divergent entries (`one-namespace-boundp`,
`reader-sharp-quote-identity`) are untouched and flip in 04D/04E.

### The symbol layout: candidate (a), measured

Two throwaway spikes (deleted before commit, like 00A's and 03A's)
measured, not asserted.  A layout spike that `#include`d `fe.c` into one
TU ran the real static `GetSymbolObjectCount`/`GetCoreObjectCount`/
`GetMinimumArenaSize` against the real object layout
(`sizeof(FeObject)=16`, `sizeof(FeEvalFrame)=96`), with a replica of
`InitializeArenaLayout`'s partition cross-checked byte-exact against the
real `OpenContext` on both the 64 KiB and 1 MiB arenas.  **Candidate (a)
is adopted**: `CDR(sym) = ((name . function) . value)`, +1 cons per
interned symbol, GC untouched, value path untouched.

| Quantity | Today | Under (a) |
|---|---|---|
| objects per symbol | `4 + (len-1)/7` | `5 + (len-1)/7` (+1) |
| `GetCoreObjectCount()` | **256** | **307** (+51) |
| `FeMinimumArenaSize()` | **53840 B** | **54656 B** (+816) |
| kg 1 MiB arena | **1100 frames / 56209 slots** | **1099 frames / 56215 slots** (frames −1, slots +6) |
| fe 64 KiB fuzz arena | **76 frames / 913 slots** | **75 frames / 919 slots** (frames −1, slots +6) |

The plan's own prediction of "frames unchanged, ≈−50 slots at open" was
**wrong in direction**, and the correction is a finding, not a failure:
the 51 extra core objects ride *inside* the grown minimum (which grows by
exactly 51 × 16 = 816 B), so the open-slot delta against a fixed 1 MiB
arena is **+6, not −50**, and one frame is lost to the rounding of the
smaller frame share.  The "today" slot count is also corrected from the
README's carried 56210 to **56209** (56210 was computed against the
pre-03F 53832-byte minimum).  `OpenContext(FeMinimumArenaSize())` still
opens at the exact boundary and `TestContextCreation` adapts by
construction, so the constant to watch is the minimum itself.

**The runtime cost is measured against kg, not cited.**  A second
spike mimicked kg's startup shape (1 MiB arena, the 78 native names,
`lisp/prelude.el`, the `lisp-arena-representative-init` forms, no-op
native bodies) and measured **228 distinct interned symbols** at the end
of the representative init, each +1 object under (a): peak live ≈ 3155 +
228 ≈ **3383 of 56215 slots (6.0%)**, a ≈16× margin.  The real counting
kg was re-run for this Decision — `utils/bench.py --case
lisp-arena-representative-init` reports `lisp_arena_peak_live` **3157**
against `lisp_arena_total_slots` **56209** (5.6%), `lisp_gc_count` 0,
`lisp_alloc_failures` 0, `lisp_frame_capacity` 1100.  The re-run number,
not the old one, is what the "no arena-size change in kg" rule is
measured against, and it says the 1 MiB arena needs no change.

### The host C API: nothing removed

kg calls no global-binding API today — no `FeSet`, no `FeIsBound`
anywhere in `src/`; its binding traffic is `FeDefineNative` (78 natives)
plus one `FeEvaluate` of a bare symbol in
`src/lisp_hooks.c:resolve_hook_function`.  `FeSet`/`FeIsBound` keep their
Emacs value-namespace meaning; `FeSetFunction`/`FeGetFunction`/
`FeIsFBound` are new in 04C, `FeGetFunction` following function-cell
symbol indirection the way call-position lookup does; `FeDefineNative`
keeps its name while its meaning changes in 04D (it registers into the
function cell), made safe by the `FE_API_VERSION` 2 → 3 bump plus kg's
`static_assert`; `FeCall`/`FeCallWithOptions`/roots are untouched.  The
parent plan's "remove the ambiguous entry points" clause resolves to
**nothing removed** — after the split, `FeSet` is unambiguously the value
namespace.  Function-cell contents may be a symbol (a designator, per the
`defalias`-late-binding case); call-position lookup follows the chain
iteratively, charged against the step budget, so a cycle dies with a
`cyclic-function-indirection` message rather than hanging.

### The migration mechanics

1. **Transitional head-resolution rule (04C)**: symbol in call position
   resolves through the function cell first, falling back to today's
   `GetBound` path — the 02B precedent, deleted by 04D, never a released
   coexistence layer.
2. **Prelude spelling (04E)**: the 53 column-zero `(setq NAME ...)` forms
   become `(defalias 'NAME ...)`; the two kg parsers that read that shape
   (`utils/check_lisp_compat.py`'s `parse_kg_prelude_defs`, and
   `test/test_lisp.c:test_prelude_source_file`) move in the same commit.
3. **`internal--let` survives** — the parent plan's §8 claim that Lisp-2
   makes it unnecessary is wrong: the Emacs `let` macro and Fe's one-binding
   `let` primitive are *both* function-namespace residents, so the
   redefinition still clobbers the cell the bodies need.  It becomes
   `(defalias 'internal--let (symbol-function 'let))` evaluated before the
   redefinition, and is deleted in Phase 8 Wave A.
4. **Bootstrap staging**: 04C leaves primitives/`fn`/math natives in value
   cells (the fallback finds them); 04D moves them to function cells,
   leaving `t`, `pi`, `e` as values; `(boundp 'car)` flips at 04D to the
   pinned snapshot.

### The funded caps

Both fe gates had exactly 29 points of headroom against a phase priced
+40 to +60, so `fe/Makefile` raises all three by the top of that range,
funding 04B–04D by name: **`SCC_COMPLEXITY_MAX` 420 → 480**,
**`SCC_FILE_COMPLEXITY_MAX` 240 → 300** (funding `fe_eval.c`, at 208
today, where the nine primitives, `funcall`/`apply`, the
`RunEvaluationLoop` head-resolution helper and designator resolution
land), and **`PMCCABE_TOTAL_MAX` 630 → 690** — pmccabe remains the
authoritative unit for the core per 03A's Decision; both units are priced
and reported anyway.  The gates were proved live against the raised
values with 03A's temporary-lowering trick: `SCC_FILE_COMPLEXITY_MAX=207`
fails on `fe_eval.c`'s 208, `PMCCABE_TOTAL_MAX=600` fails on 601, and the
raised caps pass (391/480 scc, 601/690 pmccabe).  The per-symbol pmccabe
baseline is untouched (this slice adds no code).  **kg needs no raise**:
5444/5500 re-measured, 56 points of headroom against its +15 to +25
estimate, and this slice adds no `src/*.c` to either tree — the only kg
change is the pin move in its own green commit per Rule 10.

## Status

**00A complete, 2026-08-04.** All four deliverables land: the price table
and this Decision (here); the fe.c translation-unit decision and cost
inventory (confirmed in 00A's own document, `00a-budget-and-fe-structure.md`,
cross-referenced from here); the spike (measured above, branch deleted,
no trace left in either tree). `SCC_COMPLEXITY_MAX`/`SCC_FILE_COMPLEXITY_MAX`
moved 210/105 → 220/112 in the fe submodule; kg's caps are unchanged.
`make check` and `make WITH_LISP=0 clean all check` are green in kg;
`make check`, `make complexity-check` and `make pmccabe-check` are green
in fe. No language or editor behavior changed. Sub-plan 00B may start.

**00B complete, 2026-08-04.** The Emacs-oracle differential corpus
mechanism landed in fe (`compat/{README.md,features.json,cases/,oracle/}`,
`utils/{run-emacs-oracle.py,run-fe-compat.py,check_compat_manifest.py}`,
a new `.ci/ci-09-compat.sh`) at `f23f5e1` on `analyzers-etc`, with kg's
pin moved in a separate green kg commit. Full detail, including the
tooling-path deviation from this document's suggested `tools/` (fe's own
`utils/` convention was followed instead) and every gate's demonstrated
output, is in
00B's own Status section (`00b-oracle-and-differential-corpus.md`,
removed on completion -- see git history and commit `0df3b82`).
Complexity in both trees is unchanged from 00A's landed numbers (fe
210/220 total, `fe.c` 102/112 file cap, pmccabe 197 symbols/worst 15/22;
kg 5439/5500) because this slice added no `.c`/`.h` files to either tree.
No language behavior changed. Sub-plan 00C may start.

**00D complete, 2026-08-04.** `FeGetArenaStats()` landed in fe
(`fe.h`/`fe.c`, commit `512f45f` on `analyzers-etc`) -- a read-only
accessor over eight counters already tracked at their one existing update
site each, itself allocating nothing, tested directly for that property in
`test_api.c`'s new `TestArenaStats`. fe's `SCC_COMPLEXITY_MAX` moved
210 -> 214 of the 220 cap 00A's Decision funded for exactly this work (+4
of +10, not overrun); `fe.c`'s file score moved 102 -> 106 of 112. kg's
gitlink moved to `512f45f` in a separate commit carrying the adaptation:
`src/lisp.h`/`src/lisp_core.c` expose `kg_lisp_arena_stats()` (Fe-free,
`WITH_LISP=0`-stubbed) and `kg_lisp_perf_snapshot()`, which feeds nine new
`KG_PERF_LISP_*` gauge counters (`src/perf.h`/`src/perf.c`, plus a new
`KG_PERF_SET` gauge macro) including a direct wall-clock reading of
prelude evaluation time (0.368 ms, `CLOCK_MONOTONIC`, gated behind `#if
KG_PERF_COUNTERS`). kg's scc moved 5439 -> 5444 (+5 of 61 headroom, no
raise). `utils/bench.py` gained nine Lisp cases (arena margin x3, Fe
throughput shapes x4, command latency) plus `--kg-no-lisp`/`--binary-size`
and a new `make bench-lisp-toggle` target for the with/without-Lisp and
binary-size baseline items a single counting binary cannot produce;
`test/test_perf.c` gained two Lisp shape assertions, both `WITH_LISP=0`-
safe. Every parent-plan Phase 0 baseline item has a recorded number (full
table in `00d-baselines-and-arena-observability.md`, removed on completion -- see
git history and commit `bfa9d7e`,
including the one item -- prelude time in isolation -- that looked headed
for "not measurable" until the new counter closed it directly). `make
check`, `make WITH_LISP=0 clean all check`, both complexity gates in both
trees, and kg's `.ci/ci-03` (gcc `-fanalyzer` + valgrind) and `.ci/ci-04`
(clang ASan/UBSan) are all green against this slice's final state. No
language or editor behavior changed. Sub-plan 00C (independent of 00D) may
proceed in parallel if not already underway; 01A still waits on 00C.

**00C complete, 2026-08-04.** The manifest: fe's half
(`fe/compat/features.json`, fe commit `c43fc1a` on `analyzers-etc`, kg pin
moved separately) grew from 00B's 5 proof-of-mechanism entries to 44 --
Fe's 31 primitives + 1 alias, plus 12 fe-owned cross-cutting divergences
from the parent sub-plan's day-one list (`#'`-as-identity, no integers,
`?a` unsupported, lax arity, the bounded writer, the already-agreeing
void-variable case, one-namespace `boundp`). kg's half
(`test/lisp-compat/features.json`, new) has 136 entries -- kg's 78
natives and 54 prelude definitions, plus `defcustom` (planned, Phase 8
Wave D) and three kg-owned cross-cutting divergences
(`one-namespace-clobber`, `macro-expand-every-invocation`,
`format-exceptional-float`). Every `comparison: emacs` entry in both
manifests (38 + 53 = 91 total) carries a real, version-stamped snapshot
generated and verified against `/opt-3/emacs-31-lucid/bin/emacs` during
this slice, including cross-checking all 35 initially-expected-to-agree
kg prelude definitions by evaluating the exact extracted `lisp_prelude[]`
source under standalone `fe` side by side with real Emacs. Two divergent
entries (`native-string-to-char`, `native-type-of`) were found this way,
not assumed from the parent's list. `utils/check_lisp_compat.py`
(new, wired into `make check` next to `docs-check`) adds the check that
keeps the inventory alive: every Fe primitive/alias, kg native, and kg
prelude definition parsed directly out of `fe/fe.c`/`src/lisp_prelude.c`
must be claimed by exactly one feature entry's new `source_name` field --
proven to fail on a temporary, reverted native addition. `make
lisp-compat-oracle` regenerates/verifies kg's snapshots, reusing
`fe/utils/run-emacs-oracle.py` directly. Three untested natives
(`buffer-list`, `re-search-backward`, `process-status`) are recorded as
gaps with an owner, not fixed; no native was found broken. One real
mechanism gap in 00B's own `run-fe-compat.py` (no branch for
`comparison: kg-policy`) was fixed in the same fe commit, since it
blocked `make -C fe compat` outright rather than being a native
discovered broken. `make check` (32/405), `make WITH_LISP=0 clean all
check` (337/68), and both complexity gates in both trees are green and
unchanged (kg 5444/5500, fe 214/220, `fe.c` 106/112) -- this slice adds
no `.c`/`.h` files to either tree. fe's full nine-stage
`.ci/run-ci-steps.sh` is green; kg's twelve-stage `--parallel` run is
green except a pre-existing, unrelated `ci-06` (IWYU) finding on
`src/lisp_core.c`'s unused `#include <time.h>`, introduced by the prior
00D slice's commit and not touched by this one -- reported, not fixed.
No language or editor behavior changed. Sub-plan 01A may start.

**01A complete, 2026-08-04, and with it this first sub-plan set.**  kg's
prelude is a real Lisp source file: `lisp/prelude.fe` (54 definitions) is
canonical, `utils/embed_lisp.py` generates the checked-in
`src/lisp_prelude_generated.inc`, and `src/lisp_prelude.c` drops from 377
lines to 126 with no hand-maintained Lisp left in a C string literal.
Behaviour-neutrality was proved rather than asserted, by un-escaping the
old array out of git and diffing it against the new file: 202 code lines
each, identical.  Two new targets, `lisp-prelude-generate` and
`lisp-prelude-check` (the latter inside `make check`, beside `docs-check`
and `header-check`), keep the file and the generated copy in step, and
`test_prelude_source_file` pins the definitions *and their order* --
asserting `internal--let` still answers `primitive`, which is what breaks
first if anything ever reorders the forms.  Both gates were proven to
fail, on a perturbation that was then reverted.  The prelude header's two
false claims (the obsolete "no macro may expand to bare nil" rule, and
"macros expand exactly once per call site") are corrected against `fe.c`'s
actual `FeTMacro` arm; the four `(list 'quote nil)` workarounds they
licensed are left in place, per this sub-plan's own "if in doubt" route,
since removing them changes evaluated code and Phase 2 rewrites all four
forms anyway.  kg scc 5444 -> **5443**, with `evaluate_prelude`'s pmccabe
2 -> 1 banked; fe unchanged at 214/220.  `make check` 32 native suites and
405 PTY cases and `make WITH_LISP=0 clean all check` 337 pass + 68 skip
are green, as are `header-check`, `format-check`, `docs-check`,
`lisp-compat-check` and `coverage-check`.  Full detail, including the one
prediction this document got wrong about the coverage ratchet, is in
01A's own Status section (`01a-prelude-extraction.md`, removed on
completion -- see git history and commits `440df9c`/`c73cea4`).

The `ci-06` IWYU finding 00C reported -- `src/lisp_core.c`'s unconditional
`#include <time.h>`, whose only use is behind `#if KG_PERF_COUNTERS` -- was
fixed in its own commit before this slice, so `.ci/ci-06-static-analysis.sh`
exits 0 again.

**Phase 0 and Phase 1's extraction are done.**  The contract is frozen and
measurable: there is an oracle mechanism, a 180-entry two-repository
feature inventory, a recorded performance and arena baseline, a priced
per-phase complexity table with a dated Decision, and a prelude that can
be diffed.  Nothing in the language changed, which is the point.  Phase 2
-- `setq`, `set`, and the `=` cutover -- is the next set, and it is the
first that alters behaviour.

**The first set closed green, 2026-08-04.**  All twelve CI stages passed
from an idle tree (`.ci/run-ci-steps.sh --parallel`), after two defects
the slices themselves had left behind were fixed: `src/lisp_core.c`'s
unconditional `#include <time.h>` (`4844a0d`) and `src/lisp_prelude.c`'s
missing `<stddef.h>` (`60c43ae`), both IWYU findings, the second only
visible once the compile database was regenerated.  Twelve kg commits on
`stricter-emacs-adherence` and four fe commits on `analyzers-etc`, pin at
`c43fc1a`.  The five sub-plan documents were removed at that point; this
Status section and those commits are the record.

## Status — Phase 2

**02A complete, 2026-08-04.**  Fe `ecb1110`, kg pin `bbdb608`.  Three
planned rows and 22 version-stamped Emacs 31.0.90 snapshots for `setq`,
`set` and numeric `=`.  **Every one matched this set's predicted table**,
so nothing had to be argued after the fact — which is the whole reason the
slice went first.  A defect was found and fixed on the way through (fe
`cf36951`, kg `61363f9`): `make -C fe compat-oracle`'s full run had been
broken since the corpus outgrew 00B's five cases, because it enumerated by
globbing `cases/` instead of reading `features.json` and so ran Emacs over
the six `kg-policy` cases that have no oracle answer, aborting on
`primitive-print`.  kg's `make lisp-compat-oracle` had the same failure
latent.  Both full runs now complete with no snapshot changed.  Detail in
`02a-pin-the-target-semantics.md`.

**02B complete, 2026-08-04.**  Fe `fb3e536` (+ `79a449f`), kg pin `fdd9f65`
(+ `f5e75a2`).  `setq` and `set` as core forms via two small static helpers;
assignment `=` still working; kg unchanged.  All five worked examples were
re-run through `./fe` and match the pinned snapshots, including `(2 9)` and
`((2 1) 2)`.  `make -C fe compat` 43 passed, up from 31.

**02C complete, 2026-08-04.**  Fe `9fe0220` then `841df63`, exactly the two
commits specified, plus `5cc0acb` from review.  One assignment spelling
left; `=` is chained numeric equality; `FeVersion` 2.0 and
`FE_LANGUAGE_VERSION 2` with `FE_API_VERSION` correctly unchanged.  All ten
pinned numeric answers reproduce through `./fe`.  The migration found what
no text search could: `scripts/macros.fe`'s `++` and `push` build their
expansions *programmatically* as `(list '= sym ...)`, and left alone would
have turned that file's own loop into an infinite one the moment the cut
landed.

**02D complete, 2026-08-04, and with it Phase 2.**  kg `1e3ab2a`, one
atomic 81-file commit, plus `32070b1` from review.  53-definition
`lisp/prelude.el`, three discovery paths cut to `.el` with no fallback, 52
PTY files converted, and the negative tests that are the actual deliverable
— including the `direct.fe` literal-path carve-out proving the cut is
discovery-only.  All twelve CI stages green.

### What Phase 2 cost, and the number that turned out not to mean anything

kg's estimate was zero scc and kg moved **5443 → 5444**, the one point being
a `static_assert`.  That prediction held exactly.

Fe's did not, and the reason is worth more than the estimate was.  Fe was
priced at **+20 to +30** and **scc did not move at all** — 214/220 before
02B and 214/220 after 02C, across two new special forms, a new primitive, a
deleted arm and a chained comparator.  Measured during 02B's review:
`pmccabe`'s whole-file sum for `fe.c` went **340 → 350 → 356**.  `fe.c`'s
first `'"'` character literal is at line 1010; scc's C parser desynchronizes
there and undercounts afterwards, and every line Phase 2 wrote sits past it.

So the +6 net that Phase 2 actually cost fe is a pmccabe number, and fe's
scc ratchet is not measuring the file the work happens in.  This is 00A's
spike seen from the other side — it found that *extracting* a quote-heavy
region **raised** scc by 42 with no behaviour change — and the two together
say the same thing: **fe's scc total is a floor, not a measurement.**
Phases 3–5 are priced in that unit.  Phase 3's Decision is where this has
to be settled, since Phase 3 is both the largest estimate in the table and
the slice that moves the desync by splitting the file; a pmccabe-sum ratchet
for `fe.c` is the obvious candidate and is deliberately not added here.

### Carried forward

- Three kg natives — `buffer-list`, `re-search-backward`, `process-status`
  — still have no test, recorded as gaps with an owner in
  `test/lisp-compat/features.json`.
- The three surviving `(list 'quote nil)` prelude workarounds.  02D deleted
  the fourth with the `setq` macro, as predicted; the rest are vestigial
  rather than wrong.
- **Two `(= X Y)` forms are now legitimate numeric comparisons kept on
  purpose** (`test/test_perf.c` and `utils/bench.py`, both
  `(= my-fill-column my-fill-column)`).  Anyone auditing for stale
  assignment will find them; they are documented survivors, not misses.

The first two of those came into Phase 2 from the first set and were not
closed by it; neither blocked it.  01A took its own "if in doubt" route on
the workarounds — vestigial rather than wrong, and removing them changes
evaluated code.

**Phase 3 — the frame machine — was the next set, and it was the largest
single risk in the program.**  Its seven documents (`02e`, `03a`–`03f`)
were removed once the workstream closed; the Phase 3 Status below and the
commits are the record.

## Status — Phase 3

**03A complete, 2026-08-04.**  fe `1c87e49`/`7181444`/`86e9fea` on
`analyzers-etc`, kg pin moved to `86e9fea` in its own commit.  All five
outcomes landed: the C-stack probe and its pre-change measurement table
(default/ASan-UBSan/MSan builds, linear per-level growth, and the
332/333 depth-limit boundary every build now shares safely); nine
`frame-trace-*.fe` characterization scripts with byte-for-byte goldens
generated from the unmodified recursive evaluator; a funded pmccabe
total budget (`PMCCABE_TOTAL_MAX`, 500 → 630) proved to fail on a
reverted perturbation, alongside a re-recorded per-symbol baseline
(198 → 202 symbols); the frame-storage sizing Decision above, with a
compiler-measured `sizeof(FeEvalFrame)` (80 bytes) and a peak-frame
multiplier (3 per `deep` level) that is derived from the grammar and
independently cross-validated against the probe's own empirical
332/333 boundary; and the throwaway split spike that priced all of the
above, measured and then deleted with no trace in either tree (`git
log`/`git branch` confirmed clean on both).  Full detail, including
what this slice's own document did not anticipate, is in `03a`'s own
Status section (`03a-measure-and-fund-the-frame-machine.md`, removed on
completion — see git history).  fe
complexity: 214/420 total (was 214/220 — cap raised, actual unchanged),
`fe.c` 106/240 file cap, pmccabe 500/630 total (was 500 with no total
cap), 202 baselined symbols.  kg: unchanged at 5444/5500, no `src/*.c`
touched.  `make -C fe check`, `complexity-check`, `pmccabe-check`, and
a fresh `coverage` run (88.9% lines, 65.2% branches, 80% floor) are
green in fe; kg's `make check` and `make WITH_LISP=0 clean all check`
are green after the pin move.  No language or editor behaviour
changed.  Sub-plan 03B may start.

**03B complete, 2026-08-04.**  fe `d66d2bf`, kg pin `6593bfc` (pin move plus
the full eleven-site build ripple and a `lisp-include-check` extension, in
one commit that builds — a pin-only commit would not have linked, per Rule
10).  `fe.c` split into `fe.c` + `fe_eval.c` behind a private
`fe_internal.h`, exactly the cut 03A's spike priced: pmccabe conserved
exactly (500/202 symbols before and after), scc up +72 (214 → 286) as
funded, and every per-file number — `fe.c` pmccabe 356→252, `fe_eval.c`
104/29, `fe.c` scc 106→70, `fe_eval.c` scc 108 — lands exactly on 03A's
measured prediction, not merely close to it.  No cap raised.  Full detail,
including two things this slice found that its own document did not
anticipate (`GetBound`, an accessor kept in `fe.c`, itself calls the
moving `EvaluationStep`, so the new cross-TU surface runs both directions,
not only through the kept wrappers; and the split left `fe.c` with a dead
`#include <setjmp.h>`, an IWYU finding only the full CI runner catches) and
a pre-existing, unrelated kg `ci-07-format-check` failure traced to
sub-plan 02E's `test_lisp.c` (reported, not fixed), is in `03b`'s own
Status section.  `make -C fe check`, `complexity-check`, `pmccabe-check`,
`format-check` and the full nine-stage `.ci/run-ci-steps.sh` (coverage,
gcc analyzer + Valgrind, ASan/UBSan, MSan, fuzz smoke with all three
targets linking both new objects, static analysis, format, compat) are
green in fe; kg's `make check` (32/406), `make WITH_LISP=0 clean all
check` (337/69), both complexity gates, and `.ci/run-ci-steps.sh
--parallel` (11 of 12 stages; the sole failure is the pre-existing,
unrelated one above) are green.  No language or editor behaviour changed.
Sub-plan 03C may start.

**03D complete, 2026-08-04.**  fe implementation in `fe_eval.c`/`fe_internal.h`
(fe changes uncommitted at this writing; the pin moves in kg's separate green
commit per Rule 10), test work in `fe/test_api.c`, docs in
`fe/doc/implementation.md` and this file.  The five call frame kinds plus the
native boundary are driven
by the frame stack — call-head resolution, argument evaluation, lambda
application (a synchronous transition to the body frame; `ArgsToEnv` stays an
ordinary helper as the plan requires), sequential body, macro body, macro
expansion, and the native-call boundary with the private
`native_reentry_depth` counter — while `FeFrameTemporaryRecursive` is reached
only by the special forms 03E still owns.  `FeEvalFrame` grew 80 → 96 bytes;
`FeMinimumArenaSize()` is 53832 bytes; at the retuned 10% partition kg's 1 MiB
arena holds **1100 frames / 56210 object slots** (actual 03C baseline 57542,
−1332, ≈2.3%).  Measured conversion curve,
frame-capacity actuals, the compact six-row "GC during every resumable frame
state" table (adding the previously missing call-head and macro-expansion
rows), the native shared-limit semantics, and the current complexity/coverage
numbers are in `03d`'s own Status.  scc 351/420 (03B closed at 286), pmccabe
566/221 symbols of 630 (03B closed at 500/202), coverage 90.0% lines / 95.0%
functions / 64.7% branches against the 80% floor, `make -C fe check` (native
suite plus 28 scripts × 3), both complexity/format gates, and fe's full
numbered runner in all nine stages — ci-04 (ASan/UBSan, after the
pre-existing 8 MiB `TestRootsAndCalls` stack overflow was cleared by running
with an unlimited stack limit), ci-05 (MSan, the binding lane), ci-03
(Valgrind/analyzer), fuzz smoke, static analysis, format, compat — are
green.  kg: `make check` **32/32 and 406/406**, `make WITH_LISP=0 clean all
check` **32/32 and 337 pass + 69 expected skips**, and `.ci/run-ci-steps.sh
--parallel` is **11/12** green (ci-03's one `lisp-process-cwd` PTY flake,
405/406, accepted per the documented policy after a standalone `ci-03` rerun
passed 32/32 and 406/406).  The before/after `make bench` comparison
(03C-baseline detached worktree vs current counting build) is flat to noise
and its counter evidence — peak GC stack 339→138 / 2711→1509 / 1964→763,
peak live, gc_count, allocation failures and peak eval depths unchanged — is
in `03d`'s Status.  No public
API, option, error text or statistic changed.  The 03A Decision revisit this
slice requires — the 96-byte frame accepted, the 8% -> 10% partition retune
that restored the legacy logical `(deep N)` wall at 332/333, and the
`(deep 100000)` sizing corrected to ≈275 MiB provisional — is recorded in the
Decision section above.  Special forms and the full `(deep 100000)`
flat-`deep` measurement remain 03E/03F; no full-`deep` claim is made.
Sub-plan 03E may start.

**03E complete, 2026-08-05, and with it Phase 3's evaluator conversion.**
fe `677b06b` on `analyzers-etc`, kg pin moved to match in its own separate
commit.  Every remaining special form and primitive is now a frame
continuation; `FeFrameTemporaryRecursive` and every recursive helper it was
the last caller of (`EvaluatePrimitive`, `EvaluatePair`, `EvaluateList`,
`DoList`, `EvaluateSetq`, `EvaluateSet`, `EvaluateNumericEqual`, the
`EVAL_ARG`/`ARITH_OP`/`NUM_CMP_OP` macros) are deleted — one evaluator,
reached by one path.  `unwind-protect` cleanups run through a new
`RunEvaluationBody` entry point, a nested run on the unused suffix of the
same frame stack per 03C's decision, replacing the old recursive `DoList`'s
one-nested-`Evaluate()`-call-per-form.  Every existing golden is
byte-identical, including `tests/unwind-error.fe.err` and all nine 03A
`frame-trace-*.fe` scripts — no test expectation moved.  `FeEvalFrame`
stays 96 bytes and `FeMinimumArenaSize()` stays 53832 bytes: no new struct
field, every new kind reusing existing frame fields via the established
sentinel convention.  Two bugs were found and fixed during this slice's own
development (both recorded in `fe/doc/implementation.md` and in 03E's own
Status): a double-decrement of `evaluation_depth` for frames pushed outside
`FeFrameExpression`'s own dispatch (fixed by a new, separately-completing
`FeFrameImplicitBody` kind), and — the structural concern this set's own
review flagged, confirmed real — a frames-per-level regression (4 retained
per `(deep N)` level instead of the assumed 3) that broke kg's own
`test/test_perf.c` `(dc 300)` fixture before being caught and fixed by
special-casing `if`'s common single-else-form shape, restoring the exact
`(deep 332)`/`(deep 333)` boundary 03A/03C/03D derived.  The parent plan's
actual flatness property — a flat C-stack high-water mark — is demonstrated
at N = 340 (delta 80 bytes); the literal `(deep 100000)` is not reached,
blocked by a third, previously non-binding wall this slice discovered and
recorded rather than fixed (`GcStackSize`, 4096, a fixed array field never
carved from the arena: `(deep 1021)` succeeds, `(deep 1022)` raises "GC
stack overflow" once `max_depth` is raised far enough to matter, never
under the default configuration).  Full detail, including the primitive
evaluation-order regression table and the complete new-test inventory, is
in `03e`'s own Status section (removed on completion — see git history).
fe complexity: scc 388/420 total (03D closed at 351), pmccabe 600/630 total
across 231 symbols (03D closed at 566/221), max function complexity 14.
Coverage 90.6% lines / 95.3% functions / 64.7% branches against the 80%
floor (improved from 03D's 90.0%/95.0%/64.7%).  `make -C fe check`,
`complexity-check`, `pmccabe-check`, `format-check`, a fresh `coverage` run,
and the full nine-stage `.ci/run-ci-steps.sh` (coverage, gcc analyzer +
Valgrind, ASan/UBSan, **MSan**, fuzz smoke, static analysis, format,
compat) are all green in fe from an idle tree.  kg: `make check` 32/406,
`make WITH_LISP=0 clean all check` 337 pass + 69 skip, both exactly the
audited starting counts since this slice adds no kg source beyond the pin
move and `doc/fe-upstream.md`'s divergence-table update; `.ci/run-ci-steps.sh
--parallel` is 11/12 green, the sole failure a single `lisp-process-*`
PTY case under ci-03's Valgrind lane (the documented standing flake class),
accepted after a standalone `ci-03` rerun passed 32/32 and 406/406.  No language or
editor behaviour changed.  Sub-plan 03F may start: it inherits the
`GcStackSize` finding as a concrete decision to make, the two-bounds public
API, and the `(deep 100000)` fixture (~275 MiB, unchanged since the frame
layout did not change) to turn into a permanent gated assertion once its
own blocker is resolved.

**03F complete, 2026-08-05, and with it Phase 3.**  fe `f1c0dde` and
`60e4c9e` on `analyzers-etc`; kg pin and every kg adaptation in one commit,
`50439c7`, per Rule 10 — a pin-only commit could not compile, which is
exactly what `static_assert(FE_API_VERSION == 1)` was added to guarantee.

The slice landed in three parts, and only the first was on its own document's
list.

*The blocker 03E handed over* (fe `675bcec`, kg pin `e465d21`, recorded
earlier in `03f`'s own Status): `GcStackSize` was not enlarged — 03E's other
candidate, ~3.2 MB added to `FeMinimumArenaSize()`, was never viable — because
the per-level retention turned out to be pure redundancy.  Only the run's own
result needs a `FePushGC`, once, at the barrier.  `(deep 100000)` runs, and
`peak_gc_stack_depth` is a **constant 14** at every depth measured.

*The document's own content.*  `FeEvalOptions.max_depth` split into
`max_frames` and `max_native_reentry`; `FeArenaStats.peak_evaluation_depth`
into `frame_capacity`, `peak_frame_depth` and `peak_native_reentry`;
`FE_API_VERSION` 1 → 2 and `FeVersion` "2.0" → "3.0" with
`FE_LANGUAGE_VERSION` held at 2; the two distinct error texts; and the
permanent flatness gate, which asserts a C-stack high-water **delta of 0
bytes** at N = 10, 1000 and 100000 (`peak_frame_depth` 300004 at the
deepest).  `DefaultNativeReentry` is 32: 10× the deepest nesting found in
kg's corpus (3 — `with-current-buffer` wrapping `save-excursion` under a hook
or process callback), ~1 KiB of C stack per level under ci-05's MSan flags,
so ~32 KiB against a default 8 MiB stack.

**The two-limit relationship was made structural by deleting one of the two
limits.**  This document asked 03F to "derive the relationship rather than
leave two independent numbers that happen to be close".  The honest answer
was that the second number had no remaining job: with Lisp nesting rooted in
the arena, the physical frame wall is the only wall, and `DefaultEvaluationDepth`
went with the counter it bounded.  The ~10% margin that 03D's retune already
broke once cannot recur, because there is nothing left for it to be a margin
against.

*What the slice found that no document predicted.*  fe's `ci-06` fuzz lane
caught a **use-after-free introduced by this set's own `675bcec`**.
`ResumeBinary` cleared `frame->callee` — the second operand's only collector
root — before `PCons`'s `FeCons` allocated, so a collection landing on that
allocation produced a pair with a freed cdr, which the writer reached as
`FeTFree` and aborted on.  `675bcec`'s claim that an intermediate result is
"delivered straight into the frame below's `callee` with no allocation in
between" was true of the delivery path and wrong about this one, which clears
the field first.  The per-level GC-stack pushes it removed had been masking
the hazard.  Fixed in `f1c0dde` with the rule stated where it was broken: a
frame field stays live until the last operation that might allocate has
finished with it.

Two things about that finding are worth carrying forward.  It was reachable
only through the fuzz lane — `make check`, ci-03, ci-04 and ci-05 were all
green on the broken tree, because the trigger needs the fuzz harness's 64 KiB
arena to collect often enough to land on that exact allocation.  **A green
`make check` and three sanitizer lanes are not evidence that a GC-rooting
change is safe.**  And no deterministic `test_api.c` case could pin it: the
trigger depends on accumulated arena state across many expressions, and a
sweep of arena sizes and allocation phases would not reproduce it in
isolation.  The durable guard is therefore the input itself, in a new tracked
`fe/fuzz/seeds/` directory replayed by `make fuzz-*-smoke`, because
`fuzz/corpus/` is gitignored and any guard living there dies at the next
clean checkout.

*Measured, and the number this set should watch.*  kg's arena holds
`frame_capacity` **1100** frames; the canonical chain costs ~3.02 frames per
level, so usable ordinary recursion is about **365 levels** — up from the 333
the deleted logical counter allowed, not down.  But `test_perf.c`'s `(dc 300)`
fixture peaks at **904 of 1100 frames, 82% of capacity**.  Frames per level
now converts directly and solely into user-visible recursion depth, with no
second bound masking it, so any future change that retains one more frame per
level is a proportional cut in every host's recursion depth *and* would put
that fixture over the wall.  03E already hit exactly this once.  It is
recorded in `fe/doc/implementation.md` at the `if` single-else-form special
case that exists to prevent it.

`bench.py` could not take the rename mechanically, and that is a finding
about the frame machine rather than about the benchmark.  Its nontrivial
cases asserted `lisp_peak_eval_depth > 2` to prove the key script really
reached the evaluator; frame depth does not scale with iteration count,
because a `while` body does not grow frame nesting per pass — that flatness
*is* the frame machine's property.  `lisp-arithmetic-loop` and
`lisp-macro-heavy` read the same 5 and 6 whether the loop ran 20000 times or
broke after 3, which is precisely the silent no-op the assertion exists to
catch.  Those cases moved to `lisp_gc_count` and `lisp_arena_peak_live`.

Complexity: fe scc **391/420** (03E closed at 388), pmccabe **601/630** across
230 symbols (03E closed at 600/231); the API split cost +3 and +1, and only
`AllocateFrame` and `RunEvaluation` rose.  kg unmoved at **5444/5500**.

Gates: fe's full nine-stage `.ci/run-ci-steps.sh` green from a clean tree and
a wiped fuzz corpus (ci-04 needs `ulimit -s unlimited` for the pre-existing
`TestRootsAndCalls` fixture — verified pre-existing by running the same lane
on the stashed baseline).  kg `make check` **32/406**, `make WITH_LISP=0 clean
all check` **337 pass + 69 skip**, and `.ci/run-ci-steps.sh --parallel`.  No
language or editor behaviour changed; every script, backtrace and compat
golden is byte-identical.

**Phase 3 is closed.**  Against the price table's `+42 measured split tax +
60–100 substance ≈ +100 to +140 scc` row: fe scc went 214 → 391, **+177**, and
pmccabe — the unit 03A established as authoritative for the core — went 500 →
601, **+101**, inside the estimate.  The scc overshoot is the blind spot being
paid off that the row itself predicted: the evaluator sat below `fe.c:1010`,
where scc's parser desyncs, so extracting it un-blinded complexity that was
always there.  Both units are now recorded; pmccabe is the one that means
something.

What the plan got wrong, collected: the frame size (80 → 96 bytes, 03D); the
arena partition (20% → 8% → 10%, with the 8% step breaking a kg fixture); the
frames-per-level assumption (3, briefly 4, 03E); the `GcStackSize` wall, which
no document anticipated at all until 03E hit it; and `675bcec`'s reasoning
about when a result is rooted.  Every one of them was a bound asserted rather
than measured.  The set's own lesson, earned five times: **a bound you did not
derive is a bound that moves.**

Phase 4 (Lisp-2 namespaces) followed, its price row counting on the frame
machine this phase left in place; it is now complete, and its five
documents (`04a`–`04e`) were removed once the workstream closed — the
Phase 4 Status section below is the surviving record.

## Status — Phase 4

**04A complete, 2026-08-05.**  fe `6807741` on `analyzers-etc`, kg pin
`3c7e278`.  The seventeen predicted cases and their version-stamped Emacs
31.0.90 snapshots landed in `fe/compat/` as `planned` entries; every
prediction held, including the headline `lisp2-value-function-coexist` →
`(7 9)`.  The layout spike adopted candidate (a),
`CDR(sym) = ((name . function) . value)`, +1 cons per interned symbol, GC
untouched, and corrected the plan's own "≈−50 slots at open" prediction —
the +51 core objects ride *inside* the grown minimum, so the open-slot
delta is **+6** and one frame is lost to rounding.  The dated Decision
(above) funded the phase: `SCC_COMPLEXITY_MAX` 420 → 480,
`SCC_FILE_COMPLEXITY_MAX` 240 → 300, `PMCCABE_TOTAL_MAX` 630 → 690, each
proved live with 03A's temporary-lowering trick.  No behaviour changed.

**04B complete, 2026-08-05.**  fe `c865251` then `28d1f49`, kg pin
`d9fba6c`.  Symbol layout behind accessors (`SymbolName`,
`SymbolBindingCell`, `SymbolFunction`, `SetSymbolFunction`), then the
layout change: the function cell exists, rooted and priced, read by
nothing.  All goldens byte-identical; `TestSymbolCells` pins the dormant
cell (roundtrip plus forced-GC survival).  `FeMinimumArenaSize()` 54656
bytes; kg's 1 MiB arena 1099 frames / 56215 slots at open.

**04C complete, 2026-08-05.**  fe `e6ad221`, kg pin `5cb78a3` (pin plus
the compat-manifest/checker surface that slice's plan required).  The nine
primitives (`function`, `fset`, `defalias`, `symbol-function`,
`symbol-value`, `fboundp`, `fmakunbound`, `funcall`, `apply`), designator
chains, and function-cell-first head resolution behind the transitional
value fallback — additive, so every existing script, golden and
expectation stayed unchanged, which was the slice's correctness argument.
The additive family's compat entries flipped to `supported`;
`one-namespace-boundp`/`reader-sharp-quote-identity` stayed divergent for
04D.

**04D complete, 2026-08-05.**  fe `b69ff50`, **no kg pin move** — kg
stayed on `e6ad221`, and the short fe-only window where fe was Lisp-2
against an 04C-pinned kg existed exactly as the plan said it would.  The
cut: bootstrap callables to function cells, fallback deleted, `#'x` reads
as `(function x)`, the writer prints `(function X)` as `#'X`,
`FE_API_VERSION`/`FE_LANGUAGE_VERSION` 2 → 3, `FeVersion` "3.0" → "4.0".
Both long-pinned snapshots (`one-namespace-boundp`,
`reader-sharp-quote-identity`) flip to `supported` un-regenerated, the
evidence 00C recorded the target for.  fe's full nine-stage
`.ci/run-ci-steps.sh` green.

**04E complete, 2026-08-05, and with it Phase 4.**  kg pin → `b69ff50`,
Fe 4.0 with `FE_API_VERSION`/`FE_LANGUAGE_VERSION` 3, and every kg
adaptation in one atomic commit per Rule 10.  The final state: Lisp-2
namespaces end to end — the prelude's **52 definitions** spelled
`(defalias 'NAME ...)`; the three head-position parameter calls are
`funcall`; hooks and process callbacks resolve designators through the
shared `lisp_function_designator` (`FeGetFunction`); `functionp` learned
designators; the two compile-time asserts read `FE_API_VERSION == 3` /
`FE_LANGUAGE_VERSION == 3`; natives register into the function cell,
asserted by `(boundp 'insert)` → `nil` / `(fboundp 'insert)` → `t`; both
spelling parsers moved in step; and the docs (`doc/fe-upstream.md`,
`doc/lisp-api.md`, `README.md`, `doc/kg.1`) and both compat manifests tell
the Lisp-2 truth.  **No PTY expectation was edited and no oracle snapshot
 regenerated** — the 70 Lisp PTY cases and every pinned snapshot passed
as-is, which is the slice's whole point.  `make check` **32/406**;
`make WITH_LISP=0 clean all check` **32 native, 337 pass + 69 skip** (from
the CI lane); both complexity gates green — kg scc **5444/5500,
unchanged**, and pmccabe **1246 recorded, 1 new** (`lisp_function_designator`)
**and 1 improved** (`resolve_hook_function` 2 → 1).  `.ci/run-ci-steps.sh
--parallel` is green from an idle tree; the one failure in the first run
was `ci-07-format-check` on `src/lisp_core.c`'s designator helper, fixed
since, and `make format-check` passes.

### What the plan got wrong, collected

- **53 definitions, not the parent plan's 54; 18 macros, not 22.**  The
  audited tree's numbers, which 04A recorded and 04E's `defalias` spelling
  keeps (52 after the identity lambda the real `function` special form
  replaced is deleted).
- **`internal--let` remains**, contra the parent plan's §8 claim: the
  Emacs `let` macro and Fe's one-binding `let` primitive are *both*
  function-namespace residents, so the redefinition still clobbers the cell
  the bodies need.  It is `(defalias 'internal--let (symbol-function
  'let))` evaluated before the redefinition, and is deleted in Phase 8
  Wave A.
- **The plan's literal `(function NAME)` command handoff is invalid.**
  `define-command` requires a function object and rejects a bare symbol, so
  `defun`'s interactive branch passes the closure itself —
  `(define-command 'NAME f)` — not `(function NAME)`/`#'NAME` as the plan
  spelled it.

### Carried forward

- Arity stays lax for lambdas (Phase 7); the new forms' arity is pinned by
  the oracle cases only.
- `void-function`, `void-variable` and `cyclic-function-indirection` remain
  message-level names, not signalable conditions (Phase 6).
- No local function bindings (`flet`/`labels`); a `let` binding does not
  shadow call position — the point of the cut.
- fe keeps `FeTMacro`; the `(macro . FUNCTION)` cons divergence is recorded
  (`lisp2-macro-representation`, `kg-policy`).

**Phase 4 is closed.**  Against the price table: fe was funded +60 on all
three gates and the actuals landed inside them (scc 441/480, pmccabe
643/690); kg was estimated +15 to +25 against 56 points of headroom and
needed no raise — scc is **unchanged at 5444/5500**, and the only pmccabe
movement is the one new helper and one improvement banked above.  What the
plan got wrong — the stale 53/54 and 18/22 counts, the `internal--let`
sentence, the `(function NAME)` handoff — is collected above; none of it
reached shipped behaviour.  Phase 5 (integers) is the next set, on the
ground this phase left behind.

### Post-close review, 2026-08-05

Three independent adversarial reviews ran against the closed phase — one
over the fe commits, one over the kg cutover, one over documentation and
manifest truthfulness — and the phase's own gates had caught none of
what they found, which is the finding worth keeping:

- **The funcall/apply arm shipped a reachable segfault.**  `(funcall (+))`
  dereferenced a wild pointer, because zero-operand arithmetic returned
  the `&unbound` sentinel and the EvalList delivery logic silently drops
  it — an invariant the arm's own comment asserted and no test checked.
  `(apply '+ '())` aborted the process the same way.  Fixed in fe
  `74395f3` by giving zero-operand arithmetic Emacs' identities (`(+)` →
  0, `(*)` → 1, `(-)` → 0, `(/)` → `wrong-number-of-arguments`) plus a
  defensive guard in the arm.  The same commit fixed `void-function`
  naming the wrong symbol through alias chains under `funcall` (the
  `dead` out-parameter's only consumer was the one that got it wrong),
  taught `funcall`/`apply` to reject macros and special forms with
  `invalid-function` (they previously mis-executed on quote-wrapped
  operands) behind a new additive `FeIsFunction()` API, and cut `apply`'s
  GC-stack cost from 4 slots per spread element to O(1) — a 1024-element
  spread died where a 4000-argument direct call worked.  fe `c4f50f9`
  repointed four compat rationales whose stated blockers had landed.
- **kg's designator seams could crash, and the docs promised otherwise.**
  A hook, filter or sentinel named by a symbol with an empty function
  cell produced an *anonymous* "tried to call non-callable value" where
  three documents promised a contained error naming the symbol — and the
  investigation found the promise was unkeepable by raising: an error
  raised at the seam, outside a nested evaluator run, longjmps into a
  returned frame (a pre-existing segfault, confirmed empirically).  kg
  `b4c5ddc` moved the pin and made the seam *report* instead of raise
  (`lisp_callable_designator`, `src/lisp_core.c`), naming the symbol in
  a contained `void-function`/`invalid-function` hook error; it also
  switched `functionp` to `FeIsFunction` so `(functionp 'if)` is `nil`
  as in Emacs, fixed `defun`'s interactive branch evaluating its lambda
  twice (the function cell and the command registry held two different
  closures), and repaired the small test/comment debts the review
  listed.  kg `cf70a79` rebuilt `let`'s expansion with a loop instead of
  two `mapcar`+`#'` passes — measured 2.92 s → 1.92 s over 200k
  two-binding expansions, expansion byte-identical.
- **The docs audit** (kg `24609d5`) caught the arena-figure
  self-contradiction in `doc/fe-upstream.md`, a four-document claim that
  kg's Lisp has no `unwind-protect` (it does), a stale GC-stack
  recursion bound in the document that declares itself authoritative,
  and the nine namespace forms mis-attributed to the prelude.
- **The seam fix left one hole, closed last.**  `b4c5ddc`'s
  report-not-raise seam still crashed on a *cyclic* designator chain
  (`(fset 'x 'x)` + `add-hook` + a save), because the only resolver a C
  host has, `FeGetFunction`, raised `cyclic-function-indirection` — the
  exact containment problem the rest of the seam had just been cured of.
  Emacs offers no behaviour to copy here: its `fset` refuses to *build*
  the cycle, so `indirect-function` never sees one.  fe `b864278` makes
  `FeGetFunction` answer nil for a cycle as a documented host-API design
  choice (call position, `funcall` and `apply` still raise); kg
  `47e741b` moves the pin, pins the contained behaviour in `test_hooks`
  (the old pin dies with SIGSEGV on that case; the new one reports
  `void-function` and carries on — a cycle and a dead multi-link chain
  are indistinguishable at the seam, deliberately), and flips the
  `functionp`-on-a-cycle pin from divergence to Emacs match (`nil`,
  because `functionp` now asks `FeIsFunction` about the resolved value,
  not the symbol).

Cost: fe +18 scc / +17 pmccabe (459/480, 660/690 — still inside 04A's
funding, no cap raised), kg +6 scc (5450/5500).  All gates re-green on
both sides, including fe's nine-stage runner three times and kg's full
parallel runner.  The lesson, recorded for the next set: **a slice's
"context reuse after error" tests must include the zero-operand and
empty-list degenerate of every new form** — all four fe defects lived in
degenerate inputs the happy-path suites never spelled.
