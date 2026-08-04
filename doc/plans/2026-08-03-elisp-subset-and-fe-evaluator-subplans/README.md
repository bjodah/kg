# Sub-plans — Emacs Lisp subset and Fe evaluator evolution

Parent plan:
[../2026-08-03-elisp-subset-and-fe-evaluator.md](../2026-08-03-elisp-subset-and-fe-evaluator.md),
reviewed and corrected 2026-08-04; the Phase 2 set was implementation-audited
against the same tree on 2026-08-04, and this Phase 3 set was written
against the tree as it stands after Phase 2 closed.  Read the parent's §0
(verified baseline), §0.1 (the two complexity ratchets), §0.2 (the
translation-unit decision) and §0.3 (scope honesty) before any of these;
they are the facts these documents assume.  Where the parent's Phase 3
section still names the pre-Phase-2 script/PTY counts or leaves a mechanism
at requirement level, this implementation-audited set controls; do not edit
tests back to those older counts.

Measured on the audited tree, 2026-08-04, and re-measured rather than
carried forward per Rule 6: kg **5444/5500** scc, **32 native / 405 PTY**
(**337 pass + 68 skip** under `WITH_LISP=0`), **69** Lisp PTY cases; fe
**214/220** scc with `fe.c` at **106/112**, `fe.c`'s pmccabe sum **356
across 135 symbols** (worst function `EvaluatePrimitive` at 15), all
`PMCCABE_PATHS` **500 across 202 symbols**, and
`FeMinimumArenaSize()` **43536 bytes** against kg's 1 MiB arena.

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

**This set covers Phase 3 — the frame machine — plus Phase 2's residual
debt.**  Phase 3 replaces recursive evaluation with an explicit frame
machine.  It is behaviour-neutral by design, it is the largest single risk
in the program, and it is the only phase so far whose gate is a
*measurement* rather than an assertion about output.

The through-line is that Phase 3 is a *measurement* problem before it is an
implementation one.  Three of its facts were established while auditing
this tree, and each one changes what the slices have to do:

- **Nothing in either tree can take the gate's measurement.**  The gate
  says a C-stack high-water mark must be flat across `(deep 10)`,
  `(deep 1000)` and `(deep 100000)`; the closest existing evidence is two
  crash points, and a crash point cannot show that a number stopped
  growing.  The instrument has to exist before the change, or the change
  writes its own gate.
- **fe's complexity ratchet cannot survive the file split.**  00A's spike
  measured pmccabe conserved exactly (335 → 335) and scc *up 42* across a
  purely mechanical move.  Phase 3 both splits the file and has the
  program's largest complexity estimate; measured in scc those two costs
  are inseparable.
- **One plausible frame design does not fit in kg's arena.**  A frame stack
  sized for the gate's `(deep 100000)` is roughly 4.8 MB against kg's 1 MiB
  arena and today's 43536-byte `FeMinimumArenaSize()`.  That is a design
  decision with a numeric answer, and it has to be taken before the
  substrate is written, not discovered inside it.

So the set front-loads one measurement-and-decision slice, lands the
mechanical split on its own so its cost is attributable, migrates frame
kinds in two groups behind existing test expectations, and changes bounds,
API and documentation last — deliberately, so that every slice before the
last can use "no existing test expectation changed" as its correctness
argument.

## Grouping

| Sub-plan | Phase | Focus | Prerequisites |
|----------|-------|-------|---------------|
| [02E](02e-residual-phase-2-debt.md) | 2 | Test the three untested kg natives, settle the three `(list 'quote nil)` prelude workarounds, comment the two `(= X Y)` survivors | none — runs in parallel with 03A |
| [03A](03a-measure-and-fund-the-frame-machine.md) | 3 | The C-stack probe, backtrace characterization, a split-proof complexity ratchet, the frame-storage sizing answer, and Phase 3's dated Decision | none — **this is first** |
| [03B](03b-translation-unit-split.md) | 3 | `fe.c` → `fe.c` + `fe_eval.c` behind a private `fe_internal.h`; the eleven kg build sites and the `doc/fe-upstream.md` promise | 03A (the spike must have priced this exact cut) |
| [03C](03c-frame-substrate.md) | 3 | Evaluator state, bounded context-owned frame stack, GC rooting, checkpoints and the call trace off the C stack; leaf paths only | 03B |
| [03D](03d-call-frames.md) | 3 | Head resolution, arguments, lambda, body, macro expansion, and the native-re-entry boundary; pure call chains become flat | 03C |
| [03E](03e-special-form-frames-and-unwind.md) | 3 | `if`/`and`/`or`/`while`/`let`/`setq`/`set`/`unwind-protect`, frame-aware cleanup, deleting the recursive path, and the first full flat-`deep` result | 03D |
| [03F](03f-two-bounds-the-api-break-and-the-kg-pin.md) | 3 | Split `evaluation_depth` into two bounds, rename the `FeEvalOptions`/`FeArenaStats` fields, `FE_API_VERSION` 2, every stale document, and kg | 03E |

**03A is genuinely first, for the same structural reason 00A and 02A
were.**  Phase 0 built an oracle so Phase 2's answers could be recorded
rather than argued; Phase 3 needs the same discipline applied to a
measurement.  A gate whose instrument is written by the slice that has to
pass it is not a gate.  03A also carries the two decisions — how fe's
complexity is measured from here, and where frames live — that 03B and 03C
would otherwise each take unilaterally, in the middle of a diff.

**03B is mechanical and lands alone on purpose.**  It changes no branch and
no test expectation; its correctness argument is that the pmccabe total is
conserved and every golden file is byte-identical.  Landing it inside a
frame-machine commit would make both costs one number and neither
reviewable.

**03C, 03D and 03E are one migration split at green review points.**  The
partially-migrated evaluator they pass between them is **not** the "second,
test-only evaluator mode" the parent plan forbids: one `Evaluate()`, one
entry point, no build flag or runtime switch, and the surviving recursive
helpers are reached *from a frame* and deleted as their frame kinds land.
03E's deletion checklist is what closes that out; anything left on it at
the end of 03E is either used or dead.

Do not confuse the two flatness milestones.  03D proves a generated finite
chain containing only calls no longer grows the C stack.  The canonical
`(deep N)` also contains `if` and arithmetic; it cannot go flat until 03E
converts those special-form/primitive continuations.  03F does not finish
evaluator logic: it publishes the already-working bounds and makes the
100000-depth measurement a permanent assertion.

**03F is last because it is the only slice that edits an existing
expectation.**  Bounds, error strings, public field names and
`test_recursion_depth` all move there, together with the version bump and
the kg pin.  A pin-only commit is impossible by construction:
`src/lisp_core.c`'s `static_assert(FE_API_VERSION == 1)` fires, which is
exactly the state that assertion exists to make impossible.

## Handoff contract

Give one engineer one row at a time.  The linked sub-plan's “Files this
slice owns”, test list and “does not do” section are part of the acceptance
criteria, not suggestions.

| Slice | Primary edit surface | Evidence it must add or preserve | Explicitly not its test |
|---|---|---|---|
| 02E | kg `test/test_lisp.c`, one tmux PTY, compat manifest, prelude/comments | two existing unit fixtures, one real-child status case, generated-prelude identity | no new oracle answer; no PTY for pure prelude/native logic |
| 03A | Fe test/scripts, pmccabe checker/Makefile, Decision docs | pre-change stack measurements, semantic trace goldens, failing total-budget perturbation | no evaluator implementation; no Emacs differential |
| 03B | `fe.c`/`fe_eval.c`/private header and both build systems | byte-identical Fe goldens, conserved pmccabe total, full Fe/kg build lanes | no behavioural test or golden rewrite |
| 03C | frame storage/loop/root/error substrate, arena docs | exact step pins, forced-GC roots, physical exhaustion/reuse, unchanged trace | no claim that leaf conversion makes full recursion flat; no PTY |
| 03D | call/lambda/macro/native frames | generated pure-call-chain flatness, macro exhaustion, nested-native error/reuse, per-state GC | no `(deep 100000)` claim; no special-form migration |
| 03E | remaining primitive/special-form continuations and unwind | primitive order table, every distinct resumption under GC/budget/cancel, cleanup LIFO/failure, full probe observation | no strict-arity changes and no internal-state PTYs/oracle regeneration |
| 03F | public Fe fields/version/stats, kg mirrors/perf/tests/docs | exact two-bound edges, permanent 100000 probe gate, kg frame exhaustion/recovery, full runners | no language additions, arena increase or compatibility alias |

For all rows, current suite counts are a starting census, not literals to
assert: 02E and 03A intentionally add cases.  Focused tests run while
iterating; the full Fe runner closes the Fe workstream before the final pin,
and kg's full parallel runner closes Phase 3.

## Compatibility direction

The parent's §0.4 is binding for every sub-plan: there are no known external
users of this Fe fork and no known user-written kg `init.el` files.  Here,
"compatibility" means agreement with the pinned Emacs oracle, not preservation
of the old Fe/kg dialect.

Price and implement only the hard cutovers the two repositories need.  Do not
add legacy aliases, C-API wrappers, dual-evaluator modes, source-file lint for
hypothetical configs, or `.fe` filename fallbacks.  Version numbers still move
because they make the Fe↔kg contract checkable.

For Phase 3 this cuts one specific way.  kg is the only host, so the
`FeEvalOptions`/`FeArenaStats` field renames in 03F cost nothing to keep
compatible and are worth a great deal: renaming a field whose *meaning*
changed turns a silent behaviour difference into a compile error, and kg's
`FE_API_VERSION` assertion is already positioned to catch it.  Keeping
`max_depth` under a changed meaning would be the compatibility gesture, and
it is the wrong one.

## Sequencing

```text
02E  residual Phase 2 debt (kg only, independent)


03A  measure and decide ──> 03B  the fe.c split (pin moves)
                                  │
                                  v
                             03C  frame substrate, GC rooting, leaf paths
                                  │
                                  v
                             03D  call frames  ── pure call chain is flat
                                  │
                                  v
                             03E  special forms, unwind, deletion ── full Lisp flat
                                  │
                                  v
                             03F  two bounds, FE_API_VERSION 2, kg sources
```

Strictly linear within Phase 3.  Every arrow is a real dependency: 03B
lands a cut 03A's spike has already priced, 03C needs somewhere for the
frame machine to grow, 03D consumes 03C's frames, 03E consumes 03D's, and
03F must not define bounds for a half-built machine.

02E is the only thing that can proceed in parallel.  It is kg-only, touches
nothing Phase 3 touches, and exists so that the program's largest slice
does not start with an inherited to-do list attached to it.

Every fe slice moves kg's pin in its own separate green kg commit (Rule
10).  03B changes kg build rules, 03C may update the arena-layout source
comment and documentation, and 03F is the only slice that changes kg's Fe
API mirror, counters or runtime-facing expectations.

## What this set deliberately does not do

- **No language behaviour change.**  Phase 3 is behaviour-neutral, and
  every slice's correctness argument depends on that: existing goldens,
  existing `test_api.c` expectations, existing PTY cases, unedited.  The
  one exception is 03F, whose edits are bounded to the bound names, the
  error strings, resource ceilings/statistics and field names.  “Neutral”
  means successful programs retain values, side effects, ordering and
  diagnostics below those explicit ceilings; it does not pretend a new
  physical frame limit is unobservable.  If a `comparison: emacs` compat
  case moves in any slice, that is a finding to record in the manifest with
  an oracle case, not a golden to update.
- **No second evaluator, and no evaluator mode switch.**  The parent plan
  forbids a test-only recursive mode kept alive alongside the frame
  machine, because it would duplicate every primitive and GC change made
  while the phase is in flight.  A partially-migrated single evaluator is
  a different thing, and the distinction is written into 03C.
- **No condition system.**  03C introduces the five internal completion
  kinds because the frame loop's shape depends on them, but throw, quit
  and budget-exhaustion stay *internal and unreachable*.  Nothing about
  them may appear in `fe.h`, in an error message, or in the compat
  manifest — an unreachable enumerator in a private header is cheap, a
  host-visible one is a promise.  Existing interrupt and step-limit failures
  continue to travel as ordinary errors in Phase 3; they do not make the
  future `quit`/`budget exhaustion` variants public or reachable yet.
  Conditions are Phase 6.
- **No Lisp-2, no integers.**  Phases 4 and 5.  03E may replace
  `ARITH_OP`/`NUM_CMP_OP` only as required for resumable evaluation, while
  preserving their per-operand type-check and extra-argument behaviour;
  it must not pre-empt Phase 5's mixed-type arithmetic design.
- **No third translation unit.**  03B extracts the evaluator and stops.
  The reader stays in `fe.c` with its `'"'` literals and its scc desync;
  after 03A the ratchet that matters for the core is blind to none of it.
- **No arena-size change in kg.**  If `FeMinimumArenaSize()` growth makes
  kg's 1 MiB uncomfortable, that is a finding to report with numbers, not a
  constant to quietly raise.

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
| 4 — Lisp-2 | Symbol value/function cell accessors, function-position lookup, `function`/`funcall`/`apply`/`fboundp`/`symbol-function`/`fset`/`fmakunbound`/`defalias`, `#'` reader change | ~10 small new functions at 3–6 pmccabe each | **+40 to +60** | Provisional (milestone 1) |
| 5 — integers | `FeTInteger`, reader/printer int-vs-float branches, `ARITH_OP`/`NUM_CMP_OP` macro-generated arms extended for mixed types, `eq`/`eql`/`equal`/`=` split apart | Fe's existing arithmetic/comparison macro family, roughly doubled for the mixed-type cases | **+50 to +70** | Provisional (milestone 1) |
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
| 4 — Lisp-2 | Command registry's rooted-callable lookup moves from value cell to function cell; `defun`/`defmacro` rewrite is Lisp, not C | `src/lisp_cmd.c` (57) | **+15 to +25** | Provisional (milestone 1) |
| 5 — integers | ~10 `FeMakeDouble` call sites become type-aware; printer/formatting glue | `src/lisp_buffer.c`/`lisp_word.c`/`lisp_search.c` | **+15 to +25** | Provisional (milestone 1) |
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

**Phase 3 — the frame machine — is the next set, and it is the largest
single risk in the program.**  Its seven documents (`02e`, `03a`–`03f`) are
in this directory; the Grouping and Sequencing sections above describe
them, and `03a` must land before any of the rest.

## Status — Phase 3

Not started.
