# 00A — Complexity budget and the Fe translation-unit decision

Parent: [Phase 0](../2026-08-03-elisp-subset-and-fe-evaluator.md#4-phase-0--freeze-the-contract-and-establish-baselines),
and specifically its §0.1 and §0.2.

**Prerequisites:** none.  This is the first slice of the program.

## Why this exists

The parent plan describes ten phases of work across two repositories and
did not, as first written, contain a single complexity number.  Both trees
gate on complexity in CI, and both are at or near their ceiling:

| Tree | Gate | Cap | Measured 2026-08-04 | Headroom |
|------|------|-----|---------------------|----------|
| kg | `SCC_COMPLEXITY_MAX` (`src` only) | 5500 | 5439 | 61 |
| kg | `PMCCABE_FUNCTION_COMPLEXITY_MAX` | 110 | 91 | 19 |
| fe | `SCC_COMPLEXITY_MAX` (all sources) | 210 | **210** | **0** |
| fe | `SCC_FILE_COMPLEXITY_MAX` | 105 | **102** (`fe.c`) | **3** |
| fe | `PMCCABE_FUNCTION_COMPLEXITY_MAX` | 22 | 15 (`EvaluatePrimitive`) | 7 |
| fe | `PMCCABE_NEW_FUNCTION_MAX` | 15 | — | new functions capped at 15 |

`fe/.ci/ci-01-complexity.sh` runs fe's pair, and `.ci/ci-12-subprojects.sh`
runs `make -C fe check complexity-check pmccabe-check` **from kg's tree**.
So the first commit that adds a function to `fe.c` fails kg's CI, not just
fe's.

The follow-up program's README is the evidence that this matters.  It
records three complexity Decisions (4223 → 4450 → 4750 → 5500), two
estimates missed low, one cap that sat **silently red for an entire plan**
before the next plan noticed, and a closing instruction to whoever finishes
a program: state the measured number rather than predict it again.

## Deliverables

### 1. A per-phase price table, priced against existing modules

Not a guess.  The method the follow-ups README used for Plan 06 — name the
closest existing module, say what the new work is comparable to, and give
the row a number — is the method here.  Anchors available in fe:

| Existing | scc | What it is |
|----------|-----|------------|
| `fe.c` whole | 102 | reader, writer, GC, evaluator, 31 primitives |
| `EvaluatePrimitive` | 15 pmccabe | the 31-arm primitive switch |
| `Read` | 14 pmccabe | the reader |
| `ArgsToEnv` | 13 pmccabe | parameter binding, `&optional`/`&rest` |
| `main.c` | 28 | the REPL, not compiled into kg |

and in kg:

| Existing | scc | What it is |
|----------|-----|------------|
| `src/lisp_obj.c` | 160 | the object/handle layer |
| `src/lisp_process.c` | 90 | bounded process table + natives |
| `src/lisp_cmd.c` | 57 | the command registry adapter |
| `src/lisp_buffer.c` | 67 | buffer/position natives |
| all `src/lisp_*` | 774 | of kg's 5439 |

Produce a row for every phase of the parent plan, in both trees.  The rows
that will hurt, and the honest reading of each:

- **Phase 3, the frame machine.**  Replaces a recursive `Evaluate` plus
  `EvaluatePrimitive` with an explicit stack, twelve-plus frame kinds and
  resumable states for each.  It is not plausible that this fits in 0
  points, and it is not plausible that it fits in `fe.c`'s 3 file points.
- **Phase 4, Lisp-2.**  Symbol cell layout, accessors, function-position
  lookup, `function`/`funcall`/`apply`/`fset`/`defalias`, plus kg's prelude
  and command-registry migration.
- **Phase 6, conditions.**  Five completion kinds, a condition hierarchy,
  `catch`/`throw`, `condition-case`, checkpointed cleanup — plus kg's
  translation of the new host-visible categories.
- **Phase 7, interactive arguments.**  Command metadata, an interactive
  spec parser, argument construction.  Priced against `lisp_cmd.c` (57)
  roughly doubling.

Price the hard-cut design in the parent's §0.4.  Do **not** reserve complexity
for compatibility wrappers, an assignment-`=` lint/deprecation path, a second
evaluator, `.fe` filename fallback, or a lax-arity mode.  They have no users
and are not deliverables.  `defcustom` is now a Wave D library/prelude form;
include its validation and docstring storage in Phase 8's kg row, but do not
price a Customize UI or a general property-list system.

### 2. A Decision, written where Decisions live

The follow-up program keeps its Decisions in its own README with the date,
the trigger, the number, and — crucially — the *named repayment sources*
or an explicit statement that there are none.  Do the same here, in the
parent plan or the sub-plan README, and say plainly which of these it is:

- **A raise with named funding.**  Which code gets deleted, and when.  Note
  that fe's own history offers real candidates: the recursive evaluator is
  *deleted* at the end of Phase 3, and the `setq` prelude macro, the
  `internal--let` alias, the identity `function`, and four
  `(list 'quote nil)` workarounds all go in kg.
- **A raise without funding**, stated as such.  The follow-ups README's
  2026-08-02 Decision is the model: it withdrew a promise rather than
  quietly missing it, and explained why the original premise was wrong.
- **A refusal**, in which case the program is re-scoped here rather than
  discovered to be un-landable in Phase 3.

Whatever the number, **do not set a fixed end-state cap the way the 4223
promise did.**  That promise was withdrawn for a good reason: this program,
like the last one, builds subsystems that have no ad hoc predecessor to
trade away.  Set the resting bound from what is measured at close.

### 3. The translation-unit decision for `fe.c`

`fe.c` is 2302 lines and 102 of a 105 file cap.  Phase 3 cannot land in it.
The options, with their real costs:

**(a) Raise `SCC_FILE_COMPLEXITY_MAX`.**  Cheapest, and defensible for a
single-file interpreter core — but it is the gate that keeps `fe.c` from
becoming what `def.h` became in kg, which `AGENTS.md` calls out by name.

**(b) Split the core into more than one translation unit.**  This is a
change to a documented interface, not just a build detail.  There are no
external embedders to carry forward, so every known build site can change
atomically and no single-translation-unit compatibility target is needed.
`doc/fe-upstream.md` states "kg compiles only `fe/fe.c` and its public
header `fe/fe.h`" as the embedding contract.  A split moves:

- `Makefile:377` — `$(OBJDIR)/fe.o: fe/fe.c fe/fe.h`
- `Makefile:760` — `$(TESTDIR)/fe_fuzz.o: fe/fe.c fe/fe.h`
- the coverage lanes' `FE_CFLAGS` plumbing (`Makefile:566`, `:570`) and the
  comment at `Makefile:140` about which objects were instrumented
- `make lisp-include-check`, which enforces who may include `fe.h`
- fe's own `SOURCES`/`CORE_OBJS`, `SCC_COMPLEXITY_PATHS`, `PMCCABE_PATHS`
- `doc/fe-upstream.md`'s embedding paragraph
- `WITH_LISP=0`, which must still build with none of it

**(c) Move work out of `fe.c` that was never core.**  Least likely to
help: `fex*` and `main.c` are already excluded from kg's build, and the
102 is the reader, writer, GC and evaluator — all of it core.

The recommendation is **(b), decided now and executed in Phase 3**, with
(a) as a bounded fallback for the interim commits.  A split done as part of
the frame-machine work is a rebase away from a merge conflict with every
other change; a split done as its own mechanical, behaviour-free commit is
reviewable.  But do not land the split *here* — the right seam is not
knowable until the frame machine's shape is.  What this sub-plan owes is
the decision and the cost inventory above, so Phase 3 starts with the
question closed.

### 4. A spike, thrown away

Prove (b) is mechanically possible before committing to it: on a scratch
branch, split `fe.c` at any plausible seam (the reader is the obvious one —
`Read`, `ReadList`, `ReadAtom`, `ReadWrapped`, the reader macros), make
`make -C fe check` and kg's `make check` green, measure both complexity
gates, and **delete the branch**.  Record the numbers here.  A spike that
is kept becomes a half-migration nobody finishes.

## Gates

- The per-phase table exists, every row names its anchor module, and both
  trees are covered.
- A dated Decision exists with the new caps, the funding or the explicit
  absence of it, and the translation-unit answer.
- `make complexity-check` and `make pmccabe-check` pass in both trees at
  the numbers the Decision states — run them at the start *and* the end.
- Nothing in `src/` or `fe/` changed behaviourally.  If the caps in the
  Makefiles move, that is the only code change this sub-plan lands.
- The spike branch is gone.

## What this does not do

- It does not raise a cap to make room for routine work.  Rule 6 stands:
  a slice funds itself where it can.
- It does not decide fe's future module layout beyond "one TU or more".
- It does not touch the coverage ratchet.  `.ci/coverage-baseline.json`
  has per-file floors and the Lisp adapter files will move under this
  program; that is each phase's own business, not a program-wide budget.

## Spike results (2026-08-04)

Ran the spike deliverable 4 asks for: on a scratch fe branch
(`spike-fe-c-split`, off `c41f251`), split the reader
(`Read`/`ReadList`/`ReadAtom`/`ReadWrapped`, the `rparen` sentinel, and
`FeRead` itself, which tests against that sentinel) into a new
`fe_reader.c`, behind a new private `fe_internal.h` exposing the
`FeObject`/`FeContext` layout, `CAR`/`CDR`, and the handful of helpers
(`BuildString`, `IsStringEqual`, `IsNamedSymbol`) the reader and
`ArgsToEnv` both need. Wired `fe/Makefile`'s `SRCS`/link rules and a
temporary local edit to kg's `Makefile` (`FE_OBJ` gained `fe_reader.o`;
reverted after) to compile and link both trees.

**Feasibility: yes.** `make -C fe check` (`test_api.c`, all 19
`scripts/*.fe` under normal and `-a` strict-arity passes, one-liner) is
green. kg's `make check` is green: 32/32 native suites, 405/405 PTY
cases, zero failures/skips. `header-check` and `lisp-include-check` both
pass unaffected (`fe_internal.h` lives in `fe/`, not `src/`, so
`lisp-include-check`'s grep over `src/*.c`/`src/*.h` never sees it).

**Measured numbers:**

| Metric | Before (whole `fe.c`) | After (split) |
|---|---|---|
| `fe.c` scc | 102 | 100 |
| `fe_reader.c` scc | — | 44 |
| **scc total** | **210** | **252** (+42) |
| `fe.c` file cap headroom (cap 105) | 3 | 5 (cap 105; unaffected by this slice's cap change, which was measured separately, see below) |
| pmccabe sum, `fe.c` + `fe_reader.c` | 335 | 335 (exactly conserved) |
| pmccabe max function | 15 (`EvaluatePrimitive`) | 15 (`EvaluatePrimitive`, unchanged); `Read` unchanged at 14 in its new file |
| kg `src` scc total | 5439 | 5439 (unaffected — `SCC_COMPLEXITY_PATHS` is `src` only, fe growth never counts here) |
| kg pmccabe max | 91 (`localvars_parse_footer`) | 91 (unaffected) |

The scc-total jump is not noise: it reproduced on a bare, behavior-free
mechanical move, confirmed by pmccabe's exactly-conserved 335. It is
scc's own C-string-state desync (already documented in `fe/Makefile`'s
`SCC_COMPLEXITY_MAX` comment) resurfacing complexity that the desync had
been hiding while the reader's quote-heavy code lived inside the giant
`fe.c`. This is the single most important number the spike produced;
see the README Decision for what it changes about how Phase 3's split
should be priced.

**Cleanup confirmed.** `spike-fe-c-split` deleted from the fe checkout
(`git branch -D`); fe checked back out to `analyzers-etc` at `c41f251`
before any real work landed; kg's `Makefile` reverted with `git checkout
--`; kg's `fe` gitlink reverted to `c41f251` with `git checkout --`.
`git status --short` is clean in both trees after cleanup, confirmed
before the real (non-spike) cap-raise commits below were made.

## Deliverables landed

1. **Price table** — in the sub-plan README
   ([`../README.md`](../README.md#complexity-price-table-00a-2026-08-04)),
   covering all 10 parent-plan phases in both trees, each row naming its
   anchor module.
2. **Decision** — same README, dated 2026-08-04: fe's caps raised
   210→220 / 105→112 for Phase 0's named need only; no fixed end-state cap
   set; kg's caps untouched.
3. **Translation-unit decision** — confirmed here and in the README:
   split `fe.c` (option b), decided now, executed in Phase 3; every cited
   site (`Makefile:377`, `Makefile:760`, coverage `FE_CFLAGS` plumbing at
   `Makefile:566`/`:570`, the comment at `Makefile:140`, `make
   lisp-include-check`, fe's `SOURCES`/`CORE_OBJS`/`SCC_COMPLEXITY_PATHS`/
   `PMCCABE_PATHS`, `doc/fe-upstream.md`'s embedding paragraph,
   `WITH_LISP=0`) was re-verified by symbol against this tree on
   2026-08-04 and matches the sub-plan's description.
4. **Spike** — above; branch deleted, no trace left in either tree.

## What this sub-plan could not fully satisfy

The per-phase estimates for Phases 2 and 6–10 are priced by the anchor
method (closest existing module, judged proportionally), not measured —
they cannot be measured before those phases' own implementation work
exists. This is inherent to a slice whose job is to price *unwritten*
code; the README's Decision says explicitly that these rows fund nothing
by themselves and that each phase re-prices and re-measures against its
own sub-plan when it is written, which is the same discipline the
follow-up program settled on after missing two estimates low.

## Status

**Done, 2026-08-04.** All four deliverables landed (above). Both
complexity commands pass in both trees, at the start (measured 2026-08-04,
matching the parent plan's own recorded baseline exactly) and at the end
(fe: 210/220 total, 102/112 file, both after the cap raise landed as the
sub-plan's only code change; kg: 5439/5500 unchanged). Nothing in `src/`
or `fe/` changed behaviorally. Sub-plan 00B may start.
