# Sub-plans — Emacs Lisp subset and Fe evaluator evolution

Parent plan:
[../2026-08-03-elisp-subset-and-fe-evaluator.md](../2026-08-03-elisp-subset-and-fe-evaluator.md),
reviewed and corrected 2026-08-04; each sub-plan set since has been
implementation-audited against the tree it starts from, and this Phase 8
set was written against the tree as it stands after Phase 7 closed and
its acceptance-review fixes landed (2026-08-06).  Read the parent's §0
(verified baseline), §0.1 (the two complexity ratchets), §0.3 (scope
honesty) and §0.4 (no legacy constituency) before any of these; they
are the facts these documents assume.  Where the parent's Phase 8
section names a stale or wrong fact — and the audit measured eight such
facts, starting with Wave A's inverted "move into Fe core" premise —
08A's corrections control.

Measured on the audited tree, 2026-08-06 (post-review, at the Phase 7
close), and re-measured rather than carried forward per Rule 6: kg
**5714/5730** scc (the cap moved 5660 → 5730 in the review-fix cycle,
at its measured actual), **32 native / 427 PTY** (**341 pass + 86
skip** under `WITH_LISP=0`); fe **670/760** scc with `fe.c` at **100**
and `fe_eval.c` at **453/520**, pmccabe **886/980 across 299 symbols**,
and `FeMinimumArenaSize()` **56880 bytes**, kg's 1 MiB arena
partitioning to **1097 frames / 56223 object slots**.  The compat
manifests carry **300 features across both repositories** (fe 136, kg
164; census 55 fe primitives/aliases, 81 kg natives, 53 prelude
definitions).  08A re-measures at its start, as every A-slice does.

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

**The fifth set — Phase 5, integers and the numeric surface — is
complete (2026-08-05), and with it milestone 1.**  Its five documents
(`05a`–`05e`) were removed once the workstream was accepted; the Phase 5
Status section below is the surviving record.  What that set changed is
the ground this one stands on: `FeTInteger` is a real type, `42` reads
as an integer and `42.0` prints as `42.0`, division truncates,
`arith-error` names overflow and zero-divide at message level, `eq`/
`eql` are Emacs', kg's positions are integers end to end, and
`FE_API_VERSION`/`FE_LANGUAGE_VERSION` are 4.  Closing milestone 1
triggered the price table's own obligation: the provisional milestone-2
rows (Phases 6–9) are re-priced below against real post-milestone-1
measurements.

**The sixth set — Phase 6, structured errors and non-local exits — is
complete (2026-08-06).**  Its five documents (`06a`–`06e`) were removed
once the workstream was accepted; the Phase 6 Status section below is
the surviving record, including the acceptance review that this time
ran *before* the close and sent both repositories back for fixes.  What
that set changed is the ground this one stands on: errors are condition
objects behind byte-identical messages, `catch`/`throw`/
`condition-case`/`signal`/`error`/`ignore-errors` are real,
quit/error/budget are distinct completions classified at every kg
recovery seam (`C-g` reports `Quit`), a native that re-enters
evaluation contains or re-signals through `FeTryCallWithOptions`/
`FeResignal` instead of a host `setjmp` that could not work,
`save-excursion`/`with-current-buffer` are transparent to
`condition-case`, and `FE_API_VERSION`/`FE_LANGUAGE_VERSION` are 5.

**The seventh set — Phase 7, strict arity and interactive command
arguments — is complete (2026-08-06).**  Its five documents
(`07a`–`07e`) were removed once the workstream was accepted; the
Phase 7 Status section below is the surviving record, including an
acceptance review that rejected the delivery on both sides of the pin
and sent it back for a two-repository fix cycle.  What that set changed
is the ground this one stands on: an argument list is a contract
(`wrong-number-of-arguments` with `(FUNCTION NARGS)` data,
unconditionally), `(interactive …)` is real command metadata driving
`p`/`P`/`r`/FORM argument construction, the raw prefix (`C-u`, digits,
`M--`) reaches Lisp as `current-prefix-arg`, the first Lisp→minibuffer
seam prompts for `n`/`N`/`s`/`f`/`F`/`b`/`B` with cancel-is-quit and
overflow-is-error, nested `(command-execute …)` runs inside the active
evaluator, and `FE_API_VERSION`/`FE_LANGUAGE_VERSION` are 6
(`FeVersion` "7.0").

**This set — Phase 8, the first init-file compatibility wave — is
next.**  Its five documents (`08a`–`08e`) are in this directory; the
Grouping and Sequencing sections below describe them, and `08a` leads
as every A-slice has.

The through-line is that Phase 8 makes a real `init.el` load — and
tell the truth when it cannot.  Four facts, established by auditing
this tree (the audit's 30-line representative init file and per-item
inventory are condensed into 08A), shape the slices:

- **Parent §12 is measured ~60% stale.**  Eight of Wave A's twelve
  items and ten of Wave B's seventeen already exist and agree with the
  pinned Emacs — most as prelude macros that are not fragile — and
  Wave D's "add" list (`load`/`require`/`provide`/`featurep`/init
  discovery) exists in full.  The set scopes the measured remainder,
  not the parent's wave lists; 08A records the eight corrections.
- **The worst gap is one nobody listed prominently: `t` is
  assignable.**  `(setq t nil)` succeeds and `(if t 1 2)` answers 2
  for the rest of the session — one init-file line silently corrupts
  the language.  Keywords do not self-evaluate either.  These two are
  the only genuine fe-core items in Wave A, and they are 08B.
- **The reader misreads instead of rejecting.**  `?x`, `#xff`,
  `[1 2 3]`, `#:sym` and unknown string escapes all parse today as
  innocent-looking wrong values, violating §12's own rule; the audit's
  init file dies at `(setq my/char ?x)` with `void-variable ?x`.  The
  recorded rationale for deferring char literals ("Fe has no character
  type") died with Phase 5's integers.  08C rejects first, then
  implements the measured subset, and makes `file:LINE` positions
  honest.
- **The library remainder is free.**  scc counts neither
  `lisp/prelude.el` nor its generated `.inc`; the audit prototyped the
  entire Wave B remainder, the `cond` and backquote fixes, and a
  plist-free `documentation` at a measured **+0 scc** — the cost is
  arena slots (~1.3%) and one `PRELUDE_DEFS` literal.  The audit's
  init file dies on line 3 (`setq-default`) with ten more failures
  behind it; every one is in this set's scope.

So the set front-loads one oracle-and-decision slice (08A: the
constants/reader/library/diagnostics answer tables, seven Decisions,
the eight parent corrections, the two-repository funding), protects
the constants and interns self-evaluating keywords in fe (08B), makes
the reader honest — reject, then implement escapes, char literals and
radix, then report real line numbers (08C), moves the phase's single
pin and lands the zero-cost prelude library with the init file as the
headline PTY case (08D), and finishes with `defcustom`, `format`
widths/`%c`, loader diagnostics and the phase close (08E).

## Grouping

| Sub-plan | Phase | Focus | Prerequisites |
|----------|-------|-------|---------------|
| [08A](08a-pin-the-init-file-target-and-fund-the-phase.md) | 8 | The measured init-file oracle corpus (constants/keywords, reader syntax, the library remainder's contracts, diagnostics), seven Decisions — fe-core constant protection, reject-before-implement reader policy, prelude-first library, `setq-default` as documented alias, `load-path` stays C, honest line numbers, `defcustom` deferred to 08E — the eight parent-§12 corrections, and the two-repository funding | none — **this is first** |
| [08B](08b-constants-and-keywords-in-fe.md) | 8 | fe-only: `setting-constant` for `t`/`nil`/keywords at every binding position (`setq`/`set`/`let`/lambda lists), keywords self-evaluate at intern time, `keywordp`; `FE_LANGUAGE_VERSION` 7 | 08A |
| [08C](08c-an-honest-reader.md) | 8 | fe-only: reject arms for every misread syntax first, then the shared escape table, UTF-8 `?` char literals, `#x`/`#o`/`#b` radix integers, and real `file:LINE` positions for read *and* runtime errors; `FeVersion` "8.0" | 08B |
| [08D](08d-the-pin-and-the-free-library.md) | 8 | kg: the phase's single pin move (with a measured, not hoped, adaptation audit — the reject arms and keyword self-eval are the risks), then the zero-scc prelude batch: the Wave B remainder, the `cond` and backquote fixes, `documentation`, docstring capture, `setq-default`/`kbd`; the `kgbatch` harness lands; the 30-line init file loads in a PTY case | 08C |
| [08E](08e-defcustom-format-and-the-phase-close.md) | 8 | kg-only: `defcustom` per the parent's contract, `custom-set-variables` subset, `format` widths/`%c`/`%x`/`%o`, loader diagnostics surfacing `file:LINE`, `declare` no-op; closes the phase | 08D |

**08A is genuinely first, for the same structural reason 00A through
07A were.**  Phase 8's semantics are dozens of independent oracle
questions (the audit measured most — 08A's snapshots pin rather than
discover), parent §12 needs eight corrections recorded before anyone
implements from its text, the reader policy and the constant-protection
condition are decisions with comparator consequences, and both trees'
funding must be settled — kg closed Phase 7 at 5714/5730.

**08B and 08C are the fe half, and they are ordered** — keywords must
read and evaluate (08B) before the reader slice (08C) pins escape
forms that produce them.  Both are language-visible; the language
version moves once, at 08B, and `FeVersion` at 08C.

**08D is the pin plus the free library** — one slice because the
prelude batch is only testable against the pinned fe.  Its
no-adaptation bet is *not* assumed (07C's was measured; this one has
known risks — any kg corpus line relying on a misreading breaks at the
pin), which is why 08A's verification pass audits the corpus for `?`,
`[`, `#` and `:name` spellings first.

**08E closes**, carrying the smallest new-machinery load (a prelude
macro, `format` directives, a diagnostics surface) and the phase close.

## Handoff contract

Hand the slices to one engineer one row at a time; each sub-plan's
"Files this slice owns", test list and "does not do" section are part of
the acceptance criteria, not suggestions.  The five documents are removed
on completion, once the reviewer accepts the workstream; the Status
section then records what each slice actually delivered.

| Slice | Primary edit surface | Evidence it must add or preserve | Explicitly not its test |
|---|---|---|---|
| 08A | fe/kg compat manifests/cases/snapshots, both Makefiles' caps, Decision docs, parent §12 correction block, the corpus spelling audit | fresh snapshots pinning the four answer tables (landing as known gaps), both funded raises proved live, the eight corrections recorded, the `?`/`[`/`#`/keyword corpus audit filed for 08D | no implementation, no status flips, no pin |
| 08B | fe `FeMakeSymbol`, the assignment/binding paths, the condition hierarchy, `keywordp`, versions, fuzz atoms | every Table C row native- and compat-pinned, constancy at every binding position, reachability counts for the new fuzz atoms, full nine-stage fe runner | no reader work, no kg edits, no pin |
| 08C | fe `Read`/`ReadAtom`, the escape table, line accounting in `ReadEvaluatedFile`/`EvaluateInput`, versions, fuzz dictionary | reject-before-implement (misreads become named errors first, in their own commit), every Table R row both ways, line-number tests at known lines, full nine-stage fe runner | no vectors, no printer change, no kg edits, no pin |
| 08D | kg pin + `doc/fe-upstream.md`, `lisp/prelude.el` + `PRELUDE_DEFS`, `test/kgbatch.c`, manifests | the adaptation audit's outcome recorded (measured, not hoped), per-function Table L contracts, the init file loading end-to-end in a PTY case, arena peaks before/after in the commit body, **≈+0 scc verified** | no `defcustom`, no `format` work, no new C natives beyond a checked `commandp` gap |
| 08E | kg `defcustom`/`custom-set-variables` prelude macros, `src/lisp_io.c` `format`, loader diagnostics, `declare`, docs | the 08A-pinned defcustom/format oracle rows, the broken-init `file:LINE` PTY case, the full 30-line init file verbatim, full parallel runner; closes the phase | no Customize UI/state, no `message` ring, no new fe surface |

For all rows, current suite counts are a starting census, not literals to
assert.  Run focused tests while iterating; the full Fe runner closes the
Fe workstream at 08C, and kg's full parallel runner closes Phase 8 at
08E.

## Compatibility direction

The parent's §0.4 is binding for every sub-plan: there are no known external
users of this Fe fork and no known user-written kg `init.el` files.  Here,
"compatibility" means agreement with the pinned Emacs oracle, not preservation
of the old Fe/kg dialect.

Price and implement only the hard cutovers the two repositories need.  Do not
add legacy aliases, C-API wrappers, dual-evaluator modes, source-file lint for
hypothetical configs, or `.fe` filename fallbacks.  Version numbers still move
because they make the Fe↔kg contract checkable.

For Phase 8 this cuts one specific way.  Constant protection and
keyword self-evaluation are hard cuts — no mode in which `t` is still
assignable, no keyword-as-ordinary-symbol fallback — and the reader
*rejects* what it does not implement rather than preserving today's
misreadings for any hypothetical script that relied on them.  Where kg
cannot or should not match Emacs, the divergence is *recorded*, not
papered over: `setq-default`/`setq-local` are documented aliases in a
dialect with no buffer-local variables, `load-path` stays a C array
with `add-to-load-path` as kg's spelling, `(defvar x)`'s no-value form
follows 08A's measured decision, symbol escapes and vectors stay
rejected, and prompts/`format` follow the measured Emacs rows or say
why not.  Version numbers move once each — `FE_LANGUAGE_VERSION` at
08B, `FeVersion` at 08C — and kg's `static_assert` tripwire fires at
the 08D pin, as it did in 03F, 04D, 05D, 06D and 07C.

## Sequencing

```text
08A  pin the target, correct §12, fund ──> 08B  constants + keywords,
                                                fe-only, LANGUAGE 7
                                                │
                                                v
                                           08C  the honest reader,
                                                fe-only, "8.0"
                                                ── fe workstream closes,
                                                   NO pin
                                                │
                                                v
                                           08D  the pin + the free
                                                library, kg
                                                │
                                                v
                                           08E  defcustom, format,
                                                diagnostics, kg-only,
                                                no pin; closes the phase
```

Strictly linear, and nothing runs in parallel with it: 08B and 08C
both change what a source file means, 08D's prelude batch is only
testable against their pin, and 08E's macros need 08D's keywords and
library.  Every arrow is a real dependency: 08B implements Table C,
08C implements Table R on top of keyword interning, 08D proves the
corpus against both and lands the library Table L pinned, and 08E
spends what 08D landed.

The pin discipline repeats 07's deliberately: exactly one pin move in
the whole set (08D), because the fe half is two slices that land
together and the kg half is prelude machinery with no further fe
dependency.

## What this set deliberately does not do

- **No Wave E data types.**  Vectors, hash tables, symbol property
  lists, character tables, mutable strings, embedded NUL, bignums —
  each enters only with a concrete init-file use case and its own
  oracle corpus, per the parent's own rule.  `documentation` lands
  *without* property lists (the audit proved the recorded dependency
  false).
- **No Customize.**  `defcustom` is a declaration form over `defvar`
  with inert presentation keywords and rejected semantics-bearing
  ones; no `defgroup`, `setopt`, `custom-file`, Custom state or UI.
- **No regexp Lisp surface** (`string-match`/`match-string`/
  `save-match-data`), **no `symbol-name`/`intern`/`elt`/`sort`**, and
  **no `read-*` family** — all recorded second-set candidates with the
  07E carry-overs, waiting on corpus evidence.
- **No `load-path` as a Lisp variable** and no init-discovery change;
  `add-to-load-path` stays kg's spelling, recorded.
- **No printer changes.**  Char literals are read syntax only; `?a`
  prints as 97, exactly as in Emacs.
- **No sub-form source precision.**  `file:LINE` names the top-level
  form; finer positions are recorded future work.
- **No `after-change-functions` signature change** — still the
  recorded divergence row; nothing in this set's corpus forces the
  question.
- **No compatibility residue.**  No assignable-`t` mode, no
  keyword-as-variable fallback, no misreading preserved for
  hypothetical scripts (§0.4).
- **No re-litigation of recorded Phase 4–7 residue.**  `FeTMacro`,
  `internal--let`, int64-not-bignum, `is`'s tolerant comparator, the
  native re-entry walls, the two divergent kg manifest rows
  (`catch-throw-reachability`, `condition-case-native-errors`), the
  `(FUNCTION NARGS)` rendering divergence, the 16-argument interactive
  bound and the deferred interactive codes/modifiers all stand as
  their Status sections record them.

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
  the follow-up program's sub-plan 07B (not this set's) was caught by
  the second after passing the first.  Fe has
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

## Complexity price table (00A, updated at the Phase 7 close, 2026-08-06)

Every row below is priced against the closest existing module, per
00A's method.  Only two numbers in this table are *measured* rather than
estimated: the "Phase 0 (fe)" row, which is the raise this slice actually
landed, and the "split tax" component of Phase 3, which is the throwaway
spike's result (see 00A's own document for the spike).  Everything else is
an estimate from reading the parent plan's phase section against the
shape of the nearest existing code, in the spirit of the follow-up
program's Plan 06 table — a number to fund the next concretely-scoped
piece of work against, not a prediction to be silently missed.  Phases
6–10 are milestone 2 (parent §0.3). Phases 6, 8, 9 and 10 remain
**provisional** until their own A-slice funds them; Phase 7 is now funded by
07A. They are priced here because the sub-plan asks for a row per phase, and
should be re-priced when a later slice hands back real measurements.

Hard-cut design (parent §0.4) is priced throughout: no row below reserves
complexity for a compatibility wrapper, an `=`-assignment lint/deprecation
path, a second evaluator, `.fe` fallback loading, or a lax-arity mode.

### fe tree (`SCC_COMPLEXITY_MAX`, all sources; measured 2026-08-04 baseline 210/210, `fe.c` 102/105 file cap)

| Phase | What it builds | Priced against | Estimate | Status |
|---|---|---|---|---|
| 0 — freeze & baseline | Read-only arena counters: object/free slot counts, peak live objects, GC count, hooked into `MakeObject`/`CollectGarbage` | `GetDouble`/`GetNativeFn`-shaped trivial accessors, plus a few counter increments in existing functions | **+10** (measured raise, see Decision) | **Landed. Actual +4** (210 -> 214), so 6 of the 10 funded points are unspent |
| 1 — prelude extraction | fe untouched (kg-only phase) | — | 0 | **Landed. Actual 0**, as predicted |
| 2 — hard-cut `=`/`setq` | `setq` as a core special form (pair iteration over the existing assignment path), `set` as an ordinary-semantics primitive using the global setter, and a left-to-right chained double `=` arm | `EvaluatePrimitive`'s existing assignment arm (part of its 15 pmccabe today); `PLess`/`PLessEqual` as type-checking references, but not as an arity/iteration template | **+20 to +30** (net of deleting the old assignment arm it replaces) | **Landed. Actual +6, and scc could not see it.** No cap crossed, so no Decision. scc stayed at 214/220 through both slices; `pmccabe`'s `fe.c` sum went 340 -> 350 (02B) -> 356 (02C), which is the real number. Phase 2's code sits past `fe.c:1010`, where scc's parser desyncs -- see the Phase 2 Status below |
| 3 — frame machine | Explicit evaluator frame stack, 12+ frame kinds, resumable state, GC-stack/cleanup checkpoint migration into frames, `fe.c` split into ≥2 translation units (recommended, §0.2/§3 below) | Today's recursive core (`Evaluate`, `EvaluatePrimitive` 15, `ArgsToEnv` 13, `DoList`, `EvaluateList`, `EvaluateHead`, `GetBound` ≈ 35–45 pmccabe combined) roughly doubled, **plus** the measured +42 split tax | **+42 (measured split tax) + 60–100 (frame-machine substance) ≈ +100 to +140** | **Landed.** Actual **+177 scc / +101 pmccabe** (214 → 391, 500 → 601) — the scc overshoot is exactly the blind-spot payoff the row predicted (the evaluator sat below `fe.c:1010`'s desync and was un-blinded by 03B's split; +72 of the +177 is the measured split tax), while pmccabe — the unit 03A established as authoritative — landed inside the substance estimate. Caps raised by 03A's Decision: scc 220→420, file 112→240, pmccabe 500→630 |
| 4 — Lisp-2 | Symbol value/function cell accessors, function-position lookup, `function`/`funcall`/`apply`/`fboundp`/`symbol-function`/`fset`/`fmakunbound`/`defalias`, `#'` reader change | ~10 small new functions at 3–6 pmccabe each | **+40 to +60** | **Landed inside the +60 funded raise** (04A: scc 420→480, file 240→300, pmccabe 630→690).  Phase actual: scc 391→441, pmccabe 601→643.  The post-close review fixes (see the Phase 4 Status addendum) spent a further +18 scc / +17 pmccabe of the same funding on defect repair: measured close is **459/480 and 660/690** |
| 5 — integers | `FeTInteger` in the existing `Value` union, an Emacs number lexer replacing `strtod`, shortest-round-trip float printing, either-type `ResumeArith`/chained comparators, `>`/`>=`/`/=`/`integerp`/`floatp`/`eq`/`eql`, per-function math-native return types | The tower arms extend `ResumeArith`/`ResumeBinary`/the `=` arm in place (the `ARITH_OP`/`NUM_CMP_OP` macros this row originally named died with 03E) | **+50 to +70** | **Landed, and over its funding at the top of the raise.** Funded by 05A's Decision (scc 480→540, file 300→340, pmccabe 690→760).  Actuals across 05B–05D: scc 459→**538**, `fe_eval.c` 276→**330**, pmccabe 660→**752** (+79/+92 — scc went 19 points past the funded amount, pmccabe 22 past, and the caps now sit at **2/10/8 points from full**).  Per slice: 05B +1/+3 (the "nearly free" prediction held), 05C the tower's bulk +70/+58, 05D +8/+31.  The row's mid was a floor, not an estimate.  **kg needs no raise**: 5457/5500 at 05E close (+7), pmccabe +1 on each of `format_integer`, `format_float`, `native_numberp` — banked into the baseline in 05E's commit |
| 6 — conditions | 5 completion kinds, condition hierarchy, `catch`/`throw`, `condition-case`, checkpointed cleanup | Phase 3's *measured* control-flow weight — the closest comparable is not the +70 to +100 row but the actual +101 pmccabe Phase 3 landed (plus a condition hierarchy with no predecessor) | **+80 to +120 pmccabe / scc** (re-priced 2026-08-05) | **Funded by 06A's Decision** (2026-08-05) — **cannot land before it**, and no longer before anything: the core was at 2 scc / 8 pmccabe / 10 file-cap points free, and 06A raised all three caps by the top of the row — `SCC_COMPLEXITY_MAX` 540→**680** (the tightest, at 2 points, which the price table's own instruction forgot to name), `SCC_FILE_COMPLEXITY_MAX` 340→**420**, `PMCCABE_TOTAL_MAX` 760→**900** — proved live by temporary lowering on 06A's tree. The measured starting state the raise is anchored to is 533/540, `fe_eval.c` 326/340, pmccabe 751/760; the 06B–06D actuals are recorded per slice in the Status section as they land |
| 7 — strict arity | Unconditional strict arity, arity checks per primitive/special form, plus kg's interactive metadata/argument/prompting machinery | Fe's `-a` pass already exists; per-site additions across ~50 primitives, anchored to Phase 2's measured +6 pmccabe for a chained comparator and two forms; the kg half is greenfield and priced per slice in the 07 set | fe **+50 to +80** scc/pmccabe, kg **+110 to +170** scc (audited 2026-08-06) | **Landed 2026-08-06, inside the fe band and past the kg band once the review fixes are counted.** fe: scc 654 → 694 at 07B, **670** after the review's fix cycle (net **+16**, well under the +50..80 — the review deleted more than the arity checks added); pmccabe 863 → **886** (+23) across 299 symbols; no cap re-raised beyond 07A's 760/520/980. kg: 5489 → 5656 across 07C/07D/07E (**+0/+89/+78** per slice — 07E 56% past its +30..50 price), then **+58** of review-fix work to **5714**; the 5660 cap was re-set to **5730** at its measured actual in the fix cycle's closing commit. Details in the Phase 7 Status |
| 8 — init compat waves | 08A's re-scope after the audit: fe-core is only constant protection + keywords (08B) and the reader/positions slice (08C); the library remainder is prelude at **measured ≈+0 scc**; kg's C share is `format` directives, diagnostics and pin adaptations | 08B against Phase 2's measured +6 for two forms and a primitive; 08C against Phase 5's lexer work (05C's reader share); the prelude batch against the audit's prototype (+0 scc, ~720 arena slots) | fe **+25..45** (08B) **+60..110** (08C), kg **+40..90** (08D/08E C-side) — priced by 08A, 2026-08-06 | **Sub-plan set written 2026-08-06** (`08a`–`08e`); 08A's Decision funds it at slice start from re-measured floors (fe 670/760 and `fe.c` 100/520 leave 08B+08C inside the standing caps on today's numbers; kg 5714/5730 needs a raise for the +40..90 — 08A prices it). The parent's old Wave A "move into core" scope is corrected by 08A: eight of twelve items already exist as non-fragile prelude macros |
| 9 — robustness | Arena-stat API extension, iterative/pointer-reversal GC marking (replaces recursive mark), resource-exhaustion coverage | Phase 3's lesson that a *focused* rewrite is small (03E's `if`-single-else fix, ~0 net), applied to `CollectGarbage`'s mark phase | **+30 to +50** (re-priced 2026-08-05) | Provisional (milestone 2) |
| 10 — proofs | `.fe`/`.el` proof packages and fixtures | Not C; not scc-scanned | 0 | n/a |

Milestone 1 (phases 0–5) landed at **+328 scc and +252 pmccabe** on top of
the 00A baseline (scc 210 → 538, pmccabe 500 → 752) — inside the +270 to
+330 range this table predicted, at its top, and the scc figure includes
the two measured blind-spot payoffs (03B's +72 split tax, and Phase 2's
invisible-under-the-desync arithmetic).  The milestone's real headline is
not the totals but where they left the caps: **scc 538/540, `fe_eval.c`
330/340, pmccabe 752/760 — two, ten and eight points of headroom**, all of
it consumed by phases 3–5.  Milestone 2 begins with no room, and its
phases are re-priced above accordingly: each phase's A-slice raises the caps
before implementation when its measured price requires it.  Phase 7's 07A
Decision is the first kg raise and the seventh Fe raise.  Do not read the re-priced
ranges as promises; they are the "price every phase" method anchored to
measured milestone-1 comparables instead of to today's 210 baseline, and
they inherit the same "Phase 3 carries the widest uncertainty" caveat
that the original ranges did.

### kg tree (`SCC_COMPLEXITY_MAX`, `src` only; measured 2026-08-04 baseline 5439/5500, 61 headroom)

| Phase | What it builds | Priced against | Estimate | Status |
|---|---|---|---|---|
| 0 — freeze & baseline | `test/lisp-compat/` manifest + checker | Python (`utils/`), not scc-scanned | 0 | n/a |
| 1 — prelude extraction | Deletes 4 `(list 'quote nil)` workarounds and 2 stale comment claims in `src/lisp_prelude.c`; generator is Python | `src/lisp_prelude.c` | **−3 to −5** (net decrease) | **Landed. Actual −1** (5444 → 5443).  The estimate also assumed the 4 `(list 'quote nil)` workarounds would go; 01A left them, since removing them changes evaluated code and Phase 2 rewrites all four forms anyway |
| 2 — hard-cut `=`/`setq` | Deletes a Lisp-source macro, renames `.fe`→`.el`, changes discovery string literals, and adds one compile-time language-version assertion; no new loader branch | existing `src/lisp_core.c`/`lisp_io.c`/`lisp_require.c` seams | **0 expected** | **Landed. Actual +1** (5443 -> 5444), the `static_assert` line; the estimate held. pmccabe unchanged at 1246 symbols, 0 new/gone/improved |
| 3 — frame machine | Adapts `src/lisp_core.c` call sites to the new Fe API version; no new kg-side control flow | `src/lisp_core.c` | **+10 to +15** | **Landed at ~0** — 5444/5500 at close, the predicted overestimate; the only kg change was the pin move and its own green commit per Rule 10 |
| 4 — Lisp-2 | Command registry's rooted-callable lookup moves from value cell to function cell; `defun`/`defmacro` rewrite is Lisp, not C | `src/lisp_cmd.c` (57) | **+15 to +25** | **Landed at ~0** — scc unchanged at 5444 at phase close; the post-close review fixes (designator-diagnostic seam, `functionp` via `FeIsFunction`) added **+6 → 5450/5500** |
| 5 — integers | The `lisp_position()`/`lisp_finite()` funnel becomes integer-typed, `format`'s two type gates widen, `numberp` goes two-tag, the prelude equality family is rewritten | `src/lisp_buffer.c`/`lisp_io.c`/`lisp_cmd.c` seams | **+15 to +25** | **Landed well under the estimate.** 05A confirmed **5450/5500** (50 points of headroom, no raise); 05E's close re-measure is **5457/5500** — **+7** against +15 to +25, inside existing headroom, no raise.  pmccabe: three functions +1 each (`format_integer` 5→6, `format_float` 3→4, `native_numberp` 1→2), banked into the per-symbol baseline in 05E's commit.  The funnel audit (7 constructor sites + 2 gates + 1 predicate) stands |
| 6 — conditions | Translates new host-visible completion categories at kg's error/signal boundary | `src/lisp_core.c` and callers | **+15 to +30** (re-priced 2026-08-05) | Provisional (milestone 2) — kg landed at ~0/+7 on Phases 4–5, so the old +25 to +40 top was anchored to nothing; the error boundary is a small seam |
| 7 — strict arity | Interactive argument metadata, interactive-spec parser, argument construction, nested command execution and prompt seam | `src/lisp_cmd.c` (57), with the 07A audit's greenfield machinery estimate | **+110 to +170 scc** (07A, 2026-08-06) | **Landed at +167 for the slices plus +58 for the review-fix cycle: 5489 → 5714.**  Per slice: 07C +0 (the no-adaptation bet held), 07D +89 (priced +80..120), 07E **+78 against +30..50** — the one materially missed price of the phase.  The 5660 cap was re-set to **5730** at the fix cycle's measured actual (dated Makefile comment, reasons in `7468c9a`) |
| 8 — init compat waves | `defcustom`/`custom-set-variables` prelude macros, `format` widths/`%c`/`%x`/`%o` in `src/lisp_io.c`, loader diagnostics surfacing `file:LINE`, pin adaptations, a possible `commandp`-class native | `src/lisp_io.c`'s existing directive switch; the 08D prelude batch measured ≈+0 | **+40 to +90 scc** (re-priced by the 08 set, 2026-08-06) | **Sub-plan set written**; 08A funds the raise past 5730 at slice start from the re-measured floor |
| 9 — robustness | Arena-stat diagnostics command | small, new | **+5 to +15** (re-priced 2026-08-05) | Provisional (milestone 2) — Phase 0's 00D already built the read-only stats surface, so this is a small command on top of it |
| 10 — proofs | Fixtures, PTY cases | Not `src/*.c` | 0 | n/a |

Milestone 1 (phases 0–5) landed at **+13 kg points** (5444 → 5457 —
counted from where the tree stood after the Phase 0 set's own kg
scaffolding moved the 2026-08-04 header baseline 5439 → 5444; +18
against the header figure,
including the Phase 4 review's +6), a small fraction of the +35 to +65
this table predicted — kg phases consistently cost a seam, not a
subsystem, because the numeric surface is fe's.  At 5457/5500 at close
(5461 after the post-close review's +4 — the addendum below) the tree
still holds **39 points** of headroom, which is enough for Phase 6's
small share and Phase 7's parser alone; Phase 7 and 8 together are the
first kg rows that would need their own Decision.  **kg is not the
constraint anywhere in this program; fe is, throughout** — milestone 2
starts with the fe caps nearly spent, which is the finding above.
(Phase 7 then falsified that closing claim's second half: its
interactive machinery was the first kg-heavy phase, +225 kg points
against fe's net +16, and kg's cap moved twice — 5500 → 5660 → 5730 —
while fe's three caps absorbed the phase with room to spare.  Both
trees now carry funded Decisions per phase; neither is "free".)

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

## Decision — Phase 5's numeric target, measured and funded (05A, 2026-08-05)

Taken 2026-08-05, closing sub-plan 05A.  Re-measured against the tree as
it stands after Phase 4 closed and its post-close review fixes landed, per
Rule 6: fe scc **459/480** total with `fe_eval.c` at **276/300** file cap;
pmccabe **660/690** across **248 symbols**, worst function **14**
(`DispatchPrimitive`, `RunEvaluationLoop`); kg **5450/5500** (50 points of
headroom — the plan's 5444/56 predates the review fixes, this is the
re-measured number); `FeMinimumArenaSize()` **55616 B**; kg's 1 MiB arena
**1098 frames / 56221 object slots** at open.  Both fe gates have **21 and
30 points** of headroom against a phase priced **+50 to +70** — the funding
Decision comes first, for the fourth time, and the five semantic Decisions
below pin everything 05B–05D must not re-litigate.

### The corpus: every prediction held

The answer table's 41 rows became **42 case files** in `fe/compat/cases/`
— C3 and Z1 split into a value case and an error case each, and P1
(`42.0` → `42.0`) is deliberately *not* duplicated because the existing
`reader-no-integers` case already carries that exact snapshot — all
`planned` in `fe/compat/features.json`, each rationale naming Phase 5 and
the implementing slice.  Their version-stamped Emacs 31.0.90 snapshots are
checked in under `fe/compat/oracle/`, and `make -C fe compat` is green:
**132 case(s), 76 passed, 56 known gap(s), 0 failed**, the new `num-*`
entries replaying as gaps against the double-only fe exactly as designed
(some "agree early": `num-symbol-partial-exponent` because fe's `strtod`
also reads `1e` as a symbol-like token, `num-integer-past-2^53` because the
integral-shortcut printer rounds the double to the same digits).  `make -C
fe compat-oracle` wrote the 42 new snapshots and changed nothing else —
**83 unchanged, 0 failed** — the "new snapshots only" gate.  **No
prediction was corrected by the oracle**, including the ones worth
doubting: `(/= 1 2 3)` really is `wrong-number-of-arguments` (binary `/=`
confirmed against the chained `=`), `(eql 0.0 -0.0)` really is `nil`
(Emacs' float `eql` is bit-level for signed zero), `(sqrt -1)` prints
`-0.0e+NaN` as its exact spelling, and int64 overflow really is the bignum
`18446744073709551616` in modern Emacs (A8, the divergence row).  The one
measurement surprise was the arena arithmetic below — an *fe-side* number,
not an oracle answer.

### Decision 1 — representation: `FeTInteger`, `int64_t`, enum placement (a)

`FeTInteger`, payload `int64_t i` in the `Value` union
(`fe_internal.h:79-85`), is adopted, with enum placement **(a): insert
`FeTInteger` immediately after `FeTDouble`**, so the public `FeType` reads
sanely for the next decade and the ABI-visible renumbering of `FeTPtr` and
everything after it rides the phase's own version bump (05D's
`FE_API_VERSION` 3 → 4, per 05D's plan).  The spike measured the two facts
that make this free: `sizeof(Value)` is **exactly 8 bytes on both CI
compilers** (gcc and clang) with `int64_t i` added — zero object growth —
and the tag field still has headroom (`FeTSentinel` moves 14 → 15 of the
32 tag values).  Placement (a) is compile-checked by the existing
`static_assert(FeTFex0 > FeTPtr)` (`fe.c:134`) and the exhaustive switches,
including Fex's `FexGC` (`fex.c:15-36`, no `default:`, ends in `abort()`),
which gains the integer leaf arm in 05B exactly as a double marks (a
non-pair leaf).  The accompanying slots land in 05B: `type_names[]`
`"integer"` (which makes kg's `type-of` return it with zero kg code change,
flipping `native-type-of`), a `WriteObject` arm printing `%PRId64`, and the
Fex type-name registration offsets.  GC needs nothing — an integer is a
leaf.  No new interned symbols in 05B, so `FeMinimumArenaSize()` does not
move there; it moves in 05C with the new primitive names, and the measured
figure is in the spike table below.

### Decision 2 — the equality family, and who owns each name

The manifest-disjointness constraint shapes everything: every fe primitive
name, kg native name and kg prelude definition must be claimed exactly once
across both manifests, so a new fe primitive whose name kg owns cannot land
before the kg-side deletion.  The audit confirms the plan's table and it is
adopted unchanged:

| Name | Today | Target | Slice |
|---|---|---|---|
| `is` | fe primitive; doubles ≈-equal (`IsNearlyEqual`), strings by content, else identity | stays, as fe's own broad comparator; extended to integers per this Decision — **keep the epsilon behaviour** (fe's own scripts are the constituency), recorded as a sentence in `fe/doc/language.md`; kg docs present it as fe-native, not an Emacs form | 05C |
| `eq` | kg prelude alias of `is` (`lisp/prelude.el:67`) | fe core primitive: pointer identity *or* both-integers-equal (E1 is load-bearing for kg's `memq`, `auto-fill.el:39`, PTY `111-lisp-string-natives`) | **05D** (collision: prelude alias deleted in 05E's pin commit) |
| `eql` | does not exist | fe core primitive beside `eq`: `eq`, or same-type numbers equal by bits for floats / value for integers | 05D |
| `equal` | kg prelude lambda whose atom tail is `(is a b)` | prelude rewrite: spine loop unchanged; atom tail strings → `string=`, numbers → `eql`, else `eq` | 05E |
| `string=` | kg native (`src/lisp_string.c:168`), byte equality | unchanged | — |
| `=` `<` `<=` | fe primitives (double-only) | extended in place | 05C |
| `>` `>=` `/=` | do not exist anywhere | fe core primitives | 05C |
| `numberp` | kg native (`src/lisp_cmd.c:60`) | stays kg-native, widened to two tags; fe must **not** add a `numberp` primitive | 05E |
| `integerp` `floatp` | do not exist | fe core primitives | 05C |
| `zerop` | does not exist | kg prelude one-liner over `=` (no fe spend) | 05E |
| `1+` `1-` | kg prelude lambdas | untouched — integer-preserving automatically once `+` is | — |

`Equal()`'s epsilon comparison for doubles (`fe.c:365-383`) is **not** a
template for `eq`/`eql`/`=` — those are all exact; only `is` keeps its
epsilon behaviour, and the oracle cases pin the exact members (E1–E6).
The collision consequence is already wired into the sequencing: `eq` cannot
land in 05C (the manifest checker would fire against the prelude alias), so
it waits for 05D's cut, and 05E deletes the alias in the same commit that
moves kg's pin.

### Decision 3 — reader grammar

`ReadAtom`'s bare `strtod` (`fe.c:958-961`) is replaced (05D) by an Emacs
number lexer: **integer** = optional sign, digits, optional trailing dot
(R2); **float** = digits with a fractional part and/or exponent (R3, R5);
the Emacs nonfinite spellings `1.0e+INF` / `0.0e+NaN` read as nonfinite
floats **iff** the printer emits them (Decision 4 — read/print
round-tripping stays an invariant); **everything else is a symbol**, which
un-numbers `0x10`, `inf`, `nan`, `1e` (R6–R8).  Integer literals that
overflow int64 read as a **double** (the pre-bignum Emacs behaviour), a
recorded divergence row against modern Emacs' bignum — the alternative, a
read error, would punish pasted constants.  The oracle pins every branch:
R2–R5 the two numeric shapes, R6–R8 the three symbol escapes, R9 the int64
exactness boundary, R10 signed zero.

### Decision 4 — printer representation

Integers print `%PRId64`.  Floats print the **shortest representation that
reads back to the same value**, always with a `.` or exponent — the
standard `%.{1..17}g`-until-`strtod`-round-trips loop in `EmitDouble`'s
successor, deterministic and locale-independent (~10 lines).  Nonfinite
floats adopt Emacs' `1.0e+INF` / `-1.0e+INF` / `-0.0e+NaN` spellings
(P3–P5), retiring the kg.1 §format divergence whose rationale ("fe has only
one number type") this phase deletes.  The old integral-double shortcut
(`fe.c:594-604`) dies with the cut: after 05D a bare `42` *is* an integer
and `42.0` prints as `42.0` (P1, already pinned by `reader-no-integers`).

### Decision 5 — error policy

All message-level until Phase 6, matching the standing rule: `arith-error`
for integer division by zero (A5) and for int64 overflow (A8 — a
**recorded kg-policy divergence**: Emacs promotes to bignum, fe refuses;
use `__builtin_*_overflow`, never UB).  Comparison and arithmetic type
errors say `wrong-type-argument` (C5), replacing the `expected double, got
X` texts — `CheckNumericEqualOperand`'s comment (`fe_eval.c:390-392`)
explains why the oracle comparator needs the Emacs name; the whole numeric
family now follows it.  `CheckType`'s general message survives everywhere
else.

### The funded caps

Both fe gates had exactly **21 and 30 points** of headroom against a phase
priced +50 to +70, so `fe/Makefile` raises all three by the top of that
range, funding 05B–05D by name: **`SCC_COMPLEXITY_MAX` 480 → 540**,
**`SCC_FILE_COMPLEXITY_MAX` 300 → 340** (funding `fe_eval.c`, at 276 today,
where the tower arms and the seven new primitive names land), and
**`PMCCABE_TOTAL_MAX` 690 → 760** — pmccabe remains the authoritative unit
for the core per 03A's Decision; both units are priced and reported anyway.
The gates were proved live against the raised values with 03A's
temporary-lowering trick: `SCC_COMPLEXITY_MAX=458` fails on 459,
`SCC_FILE_COMPLEXITY_MAX=275` fails on `fe_eval.c`'s 276,
`PMCCABE_TOTAL_MAX=659` fails on 660 (and `make pmccabe-baseline` refuses
to launder the over-budget tree, leaving the manifest untouched), and the
raised caps pass (459/540 scc, 276/340 file, 660/760 pmccabe).  The
per-symbol pmccabe baseline is untouched (this slice adds no code).  **kg
needs no raise**: 5450/5500 re-measured, 50 points of headroom against its
+15 to +25 estimate, and this slice adds no `src/*.c` to either tree — the
only kg change is the pin move in its own green commit per Rule 10.

### The data spikes (throwaway, deleted before commit)

Three throwaway files, kept off the tree, measured rather than asserted —
the 00A/03A/04A pattern:

**`sizeof(Value) == 8` with `int64_t i`, on both CI compilers.**  A
standalone TU replicating the union exactly as `fe_internal.h:79-85`
defines it plus the proposed member: gcc and clang both report
`sizeof(Value) = 8` today and `8` with `int64_t i`, alignment 8 unchanged.
Zero object growth — the plan's claim, now compiler-measured.

**The exact-step pins (the arithmetic step-accounting exposure).**
`TestEvaluationControl` pins `"(+ 1 2)"` at **exactly 4 steps**
(`test_api.c:672-688`: step_limit 3 fails, 4 succeeds) — the headline
pin, a numeric form, which 05C carries forward **unedited** as its guard
that the tower does not change arithmetic's step accounting.  The other
exact-step pins are the tight/ok pairs in the three frame tests:
`(add-exactly 1 2)` exactly **6** (`:3084/:3089`), `((fn (x) x) 1)` exactly
**9** (`:3102/:3107`), `((fn (x) (let y 1) (list x y)) 5)` exactly **20**
(`:3254/:3258`), and `(m)` with `(macro () 5)` exactly **5**
(`:3465/:3469`).  All five carry numeric literals as arguments or in their
bodies; their step charge is per form/symbol/argument — never per numeric
type — so the tower must keep the charge identical for integer operands,
and all five pins survive 05C/05D unedited.  (The plan's cited line numbers
3074/3088/3236/3448 map to these same tests; the tree has drifted a few
lines since the plan was written.)

**Arena deltas for the planned primitive names** (`>`, `>=`, `/=`,
`integerp`, `floatp` from 05C; `eq`, `eql` from 05D).  A spike that
`#include`d `fe.c` ran the real static `GetCoreObjectCount`/
`GetMinimumArenaSize`/`GetSymbolObjectCount` against the real name arrays
(no code change), with a replica of `InitializeArenaLayout`'s partition
cross-checked byte-exact against the real `OpenContext` on both the 1 MiB
and 64 KiB arenas (MATCH on frame capacity and object count for each), and
projected the seven names through the same arithmetic the primitive loop
applies:

| Quantity | Today | With the 7 names |
|---|---|---|
| objects per name | — | `1 + 5 + (len-1)/7`; **6 objects (96 B)** for `>` `>=` `/=` `floatp` `eq` `eql`, **7 (112 B)** for `integerp` (its 8-char name crosses the 7-char string-cell boundary) |
| `GetCoreObjectCount()` | **367** | **410** (+43) |
| `FeMinimumArenaSize()` | **55616 B** | **56304 B** (+688 = 43 × 16) |
| kg 1 MiB arena | **1098 frames / 56221 slots** | **1097 frames / 56225 slots** (frames −1, slots +4) |
| fe 64 KiB fuzz arena | **74 frames / 925 slots** | **73 frames / 929 slots** (frames −1, slots +4) |

The plan's "≈96 bytes per name" prediction holds exactly, and the same
finding as 04A recurs, in the same direction: the extra core objects ride
*inside* the grown minimum, so against a fixed arena the open-slot delta is
**+4, not −43**, and one frame is lost to the rounding of the smaller
remainder.  `OpenContext(FeMinimumArenaSize())` still opens at the exact
boundary and `TestContextCreation` adapts by construction, so the constant
to watch is the minimum itself: **55616 → 56304 B**, recorded for 05C to
carry through `lisp-compat-check` the way 04A's arena figure was.

### Parent-plan corrections (recorded)

Verified against the audited tree, 2026-08-05, controlling over the parent
plan's Phase 5 section and the set README's earlier claims:

- **"ten call sites" undersells the funnel.**  There are exactly 10 raw
  `FeMakeDouble`/`FeToDouble` lines in `src/lisp_*.c`, but 13
  position-returning natives share one constructor, `lisp_position()`
  (`src/lisp_buffer.c:181`), and every numeric *argument* funnels through
  `lisp_finite()` (`src/lisp_buffer.c:115`).  The kg cutover is a handful
  of choke-point edits plus two `format` type gates (`src/lisp_io.c:82`,
  `:115`) — smaller than the parent feared, and 05E's checklist is written
  from the funnel, not the grep.
- **`ARITH_OP`/`NUM_CMP_OP` no longer exist** — deleted with the recursive
  evaluator (03E); only their names in comments survive.  The tower extends
  `ResumeArith`/`ResumeBinary`/the `PNumericEqual` arm, not macros.
- **§0.2's "split fe.c" is long settled** (03B).
- **`=` is already chained-numeric** with `wrong-number-of-arguments` and
  `wrong-type-argument` pinned (Phase 2, plus the zero-operand identities
  `(+)`→0, `(*)`→1, `(-)`→0, `(/)`→error).  The parent's `(= 3 2)`/
  `(setq x 3)` acceptance cases pass today.

## Decision — Phase 6's completion target, pinned and funded (06A, 2026-08-05)

Taken 2026-08-05, closing sub-plan 06A.  Re-measured at slice start per
Rule 6: fe scc **533/540** (`fe_eval.c` **326/340**, `fe.c` 96) and pmccabe
**751/760 across 263 symbols**, worst function 14; kg **5461/5500** (39
points of headroom).  All three fe gates sit within a row re-priced **+80 to
+120** (anchored to Phase 3's *measured* +101 pmccabe as the closest
control-flow comparable, the condition hierarchy having no predecessor), so
the funding Decision comes first, for the fifth time -- and it names all
three caps, including the scc total the price table's own instruction
forgot, which is the *tightest* at 2 points free.

### The corpus: the measured answer table, frozen

39 `cond-*` case files plus version-stamped Emacs 31.0.90 snapshots landed
as `planned` in `fe/compat/`, one per load-bearing measurement of the
audit's answer table (CC1-CC11, CT1-CT7, S1-S4, U1-U3, Q1, H1-H2, plus the
three data-shape rows), the multi-measurement rows expanded to one case
each (CC10's three quit-catchability answers, CT5's four tag-`eq` answers,
U1's three paths), and every rationale naming Phase 6 and the implementing
slice.  `make -C fe compat` is green: **181 case(s), 119 passed, 62 known
gap(s), 0 failed**, the new `cond-*` entries replaying as gaps against the
featureless fe exactly as designed (one "agrees early": `cond-q1`'s
`void-function error` message contains the oracle's condition name).
`make -C fe compat-oracle` wrote the 39 new snapshots and nothing else --
**174 unchanged, 0 failed** -- the "new snapshots only" gate.  **No
prediction was corrected by the oracle**; snapshot generation confirmed the
audited table.  The one spelling worth recording: CC2's pinned answer is
`(error "boom")` -- the table's `(error boom)` shorthand dropped the string
quotes, and the case note says so.

### Decision 1 — the condition object and the static hierarchy

A condition is the cons `(SYMBOL . DATA-LIST)` constructed at signal time
-- the shape fe already almost spells in its message text -- pinned by the
`condition-object` data rows (`(car 5)` → `(wrong-type-argument listp 5)`,
`(/ 1 0)` → `(arith-error)` with no data, `(error "fmt %d" 7)` →
`(error "fmt 7")` formatted at signal time).  The hierarchy is a **static
table in fe** (parent: "without requiring general symbol properties"): an
array mapping condition symbol → parent chain, initialised at bootstrap
with the parent's initial list (`error`, `wrong-type-argument`,
`wrong-number-of-arguments`, `void-variable`, `void-function`,
`args-out-of-range`, `arith-error`, `file-error`) plus fe's own residents
(`cyclic-function-indirection`, `invalid-function`, `no-catch`) and the two
resource conditions the parent names: `evaluation-stack-exhaustion` and
`arena-exhaustion` (Emacs' nearest analogues are `excessive-lisp-nesting` and
nothing).  Every chain is depth ≤ 2
(`X → error`); Emacs' one deeper chain (`overflow-error → range-error →
arith-error`) is out of scope until a producer needs it.  `quit`/`budget`
do **not** appear -- they are completion kinds, not signalable symbols, and
`(signal 'quit nil)` constructs an ordinary *condition* named `quit` (the
real C-g path is a distinct completion).  **Where the table lives is the
open question, to be measured rather than asserted**: `fe.c` bootstrap vs a
new `fe_cond.c` -- §0.2's split precedent (03B: +72 scc, pmccabe conserved)
says the split tax is real, and the scc raise above absorbs it if the
choice lands that way; 06D decides with a measurement, per this slice's
funding note.

### Decision 2 — what `FeHandleError`'s 97 sites become

The census: 27 sites already spell an Emacs condition name at the head of
their message (9 `wrong-number-of-arguments`, 9 `arith-error`, 5
`wrong-type-argument`, 3 `void-function`, …); 70 are bare prose; 2 are
computed (`CheckType`'s `expected %s, got %s`).  The condition-named sites
become structured signals of that condition; the prose sites become
`(error "prose")` -- Emacs' own shape for `(error "…")` -- with **no mass
rewording** (§0.4 has no legacy constituency, but 30 goldens and 109 kg
test assertions pin the prose; the message *text* survives as the
condition's rendered form through the existing label/offset formatter every
golden embeds).  `CheckType` becomes a `wrong-type-argument` producer.
kg's 112 raise sites stay prose-`error` in Phase 6 -- they gain nothing
from classification until kg Lisp can catch (06E) -- recorded in TODO, not
in scope.

### Decision 3 — condition-case scope

Handlers by symbol, by list, and `t`; nil and non-nil `var`; the handler
body as an implicit progn; unmatched re-signal; and quit catchable only by
name or `t`, never by `error` (the measured Emacs rule, pinned by the
`cond-cc10*` rows).  **`:success` (CC11) is deliberately deferred** --
recorded as a divergence row with its measured answer `(succ 5)`: it is new
in Emacs 24.1+ and nothing in kg's target init-file corpus uses it (verify
against Phase 8's wave list before recording it as anything else).
`(debug error)` handler specs are accepted and treated as `error` (Emacs
semantics without a debugger).  `ignore-errors` joins the kg prelude in
06E, not fe.

### Decision 4 — the cleanup-raise policy

Three candidate behaviours were on the table: fe today (print to stderr and
continue with the original), the design doc's claim (discard silently),
Emacs (the new error replaces).  **Match Emacs -- the new error replaces the
in-flight completion**, because the phase's whole point is that handlers can
rely on Emacs semantics, and the divergence is observable from Lisp
(`condition-case` around a failing cleanup).  The oracle is pinned by
`cond-u2`: `(error "cleanup")` wins over `(error "orig")`, and a cleanup's
`throw` likewise wins over an in-flight throw.  The design doc's "Emacs
discards" claim is false (measured), and `fe/doc/unwind-design.md` is
corrected in this slice.  06D rewrites the three `test_api.c` stderr
assertions (the `CHECK(strstr(captured, "cleanup error") ...)` family,
`:3086/:3155/:3204`) and `error_fn`'s never-sees-cleanup-failures contract
with it.  `cond-u3` pins the ordering half: nested unwind-protect runs
innermost-first.

### Decision 5 — the host API and the standalone channel

The additive host surface (the parent requires a migration path): keep
`FeErrorFn` as-is for existing hosts; add `FeGetCompletion(ctx)` +
`FeGetCondition(ctx)` (kind + condition object valid for the duration of
the callback), so kg's `handle_error` upgrades by reading, not by re-signing.
Quit and budget reach the host as distinct kinds and are **not** catchable
by `condition-case`-by-`error`.  The standalone `fe` binary needs a
structured error channel for the compat runner: today `run-fe-compat.py`
derives "condition" purely from the exit code and greps the message
(`condition_source: "message"`); 06D gives the binary a way to print the
condition symbol so `condition_source` becomes `"structured"` and
`planned-quit-signal`'s checked-in `{"kind": "quit"}` oracle becomes
matchable.  The exact printing contract is decided in 06D so the runner and
the binary move together.

### The funded caps

Measured at slice start (above): fe scc 533/540 with `fe_eval.c` at
326/340, pmccabe 751/760 across 263 symbols.  The row prices **+80 to
+120**, anchored to Phase 3's measured +101 control-flow weight, so
`fe/Makefile` raises all three by the top of that range, funding 06B-06D by
name: **`SCC_COMPLEXITY_MAX` 540 → 680**, **`SCC_FILE_COMPLEXITY_MAX` 340 →
420** (funding `fe_eval.c`, at 326 today, where the catch frame, the
handler arms and the raise-site cut land), and **`PMCCABE_TOTAL_MAX` 760 →
900** -- pmccabe remains the authoritative unit for the core per 03A's
Decision; both units are priced and reported anyway.  The gates were proved
live against the raised values with the 03A/05A temporary-lowering trick,
run on this slice's tree:
`SCC_COMPLEXITY_MAX=532` fails on 533, `SCC_FILE_COMPLEXITY_MAX=325` fails
on `fe_eval.c`'s 326, `PMCCABE_TOTAL_MAX=750` fails on 751 ("total
complexity 751 exceeds funded budget 750 (+1)"), and `make pmccabe-baseline`
refuses to launder the over-budget tree (exit 2, `.ci/pmccabe-baseline.json`
untouched); the raised caps pass (533/680 scc, 326/420 file, 751/900
pmccabe).  The per-symbol pmccabe baseline is untouched (this slice adds no
code).  **kg needs no raise**: 5461/5500 re-measured, 39 points of headroom
against a +15 to +30 row, and this slice adds no `src/*.c` to either tree.

### Parent-plan and design-doc corrections (recorded)

The six-item corrections list from the phase doc, verified against the
audited tree and the pinned Emacs 31.0.90: `fe/doc/unwind-design.md`'s
"Emacs discards" claim is false (Decision 4's oracle); the completion enum
already exists three-fifths dead (`FeCompletion`, `fe_internal.h:371`) and
the `CleanupFrameReserve` gate (`fe_eval.c:656`) is a live coupling 06B
asserts rather than leaves accidental; the checkpointed drain
`RunCleanupsDownTo` (`fe_eval.c:132`) already exists and 06C unwinds on it;
`args-out-of-range` and `file-error` have zero producers today and enter
the static hierarchy anyway (no fe test can exercise them until a producer
exists); a Lisp-callable `signal`/`error` cannot be an ordinary native (the
raise *is* the primitive's behaviour -- an evaluate-then-raise form); and
`(/ 1.0 0)` is `1.0e+INF`, not `arith-error` -- Phase 5 already matches and
the phase must not "improve" float division while wiring `arith-error` up
as a real condition.  Both the parent plan's Phase 6 section and the design
doc are corrected in this slice; the design doc also records Phase 5's
residue for the first time (`arith-error` and int64-overflow at message
level) and points its token/cancel cleanup registry (order-of-work item 2)
at Phase 9's robustness scope.  The existing `errors-and-non-local-exits`
rows were audited: `signal-and-quit`'s rationale already names Phase 6 and
stays `planned` until 06D; `void-function-error`'s
`condition_source: "message"` note is the thing 06D upgrades to
`"structured"`.  **Feature-name noncollision re-verified at slice start**:
the five names Phase 6 adds (`catch`, `throw`, `condition-case`, `signal`,
`error`) collide with no kg native, no kg prelude definition, and no
manifest id on either side -- the disjointness checker is the tripwire at
the eventual pin, and this verification is recorded here per the phase
doc.  kg: **nothing** -- no pin, no source.

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
 regenerated** — the 69 Lisp-gated PTY cases and every pinned snapshot passed
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

## Status — Phase 5

**05A complete, 2026-08-05.**  fe `59db1b9` on `analyzers-etc` (compat
corpus, caps — see below), with kg's pin moved to it in this companion
green commit per Rule 10.  The 42 planned `num-*` cases and their version-stamped Emacs
31.0.90 snapshots landed in `fe/compat/` — one per answer-table row, C3/Z1
split into value+error cases, P1 covered by the existing
`reader-no-integers` case — every prediction held (including `(/= 1 2 3)`
→ `wrong-number-of-arguments`, `(eql 0.0 -0.0)` → `nil`, `(sqrt -1)` →
`-0.0e+NaN`, A8 → bignum).  The five Decisions (representation/enum
placement (a), the equality-family ownership table, reader grammar,
printer representation, error policy) and the funding raise are in the
Decision section above: `SCC_COMPLEXITY_MAX` 480 → 540,
`SCC_FILE_COMPLEXITY_MAX` 300 → 340, `PMCCABE_TOTAL_MAX` 690 → 760, each
proved live with 03A's temporary-lowering trick (458 fails on 459, 275 on
276, 659 on 660).  The three throwaway spikes measured the free
representation (`sizeof(Value)` stays 8 with `int64_t i`, both CI
compilers), the exact-step arithmetic pins (five, led by `(+ 1 2)` at
exactly 4 steps, all numeric-form-carrying, all to survive 05C/05D
unedited), and the arena delta for the seven planned primitive names
(core 367 → 410, `FeMinimumArenaSize()` 55616 → 56304 B, kg 1 MiB
1098/56221 → 1097/56225).  `make -C fe check`, `complexity-check`,
`pmccabe-check` and `compat` are green; `make -C fe compat-oracle` wrote
42 snapshots and regenerated nothing.  No language or editor behaviour
changed.  Sub-plan 05B may start.

**05B complete, 2026-08-05.**  fe `af18906` on `analyzers-etc`, kg pin
moved to it in its own green commit per Rule 10.  `FeTInteger` exists
and is dormant exactly as the slice promised: `int64_t i` in the `Value`
union (`sizeof(Value)` still 8), `FeMakeInteger`/`FeToInteger`,
`type_names[FeTInteger]` = `"integer"`, the writer's `%PRId64` arm, the
widened `FeToDouble`, `FexGC`'s leaf arm, and `TestInteger`'s roundtrip/
forced-GC survival pins — **producible by no Lisp program**, which the
byte-identical goldens and the still-planned compat rows prove.  The
"nearly free" prediction held: fe scc 459 → **460** (+1), `fe_eval.c`
276 unchanged, pmccabe 660 → **663** (+3) — 05A's funding untouched.
`FeMinimumArenaSize()` unmoved at 55616 (no new interned symbols), as
05B's own doc required it to be verified, not assumed.  No language or
editor behaviour changed.  Sub-plan 05C may start.

**05C complete, 2026-08-05.**  fe `439d654` on `analyzers-etc`, kg pin
moved in its own green commit (carrying the five new primitive names
`>`, `>=`, `/=`, `integerp`, `floatp` through `lisp-compat-check`, per
the pin's documented tripwire).  The tower landed through the host-API
seam: integer-preserving arithmetic with `__builtin_*_overflow`
`arith-error` checks, truncating division, chained variadic `<`/`<=`
joined by `>`/`>=`, binary-only `/=`, `integerp`/`floatp` leaves, the
`is`/`Equal()` integer arm, per-function math-native return types, and
the unary `-`/`/` seed and `wrong-type-argument` behaviour changes 05C
was explicitly allowed to make — while the reader still produced only
doubles, so every script, golden and exact-step pin stayed byte-identical
including `TestEvaluationControl`'s `(+ 1 2)` at exactly 4 steps.
Actuals: fe scc 460 → **530** (+70), `fe_eval.c` 276 → **332**, pmccabe
663 → **721** (+58) — the tower was the phase's bulk, as priced, and at
the top of the row.  `make -C fe check`, `complexity-check`,
`pmccabe-check`, `format-check`, `compat` (the arithmetic/comparison/
predicate/math rows flipping to `supported`) and fuzz smoke are green.
No kg source changed.  Sub-plan 05D may start.

**05D complete, 2026-08-05.**  fe
`17ac959` on `analyzers-etc`, **no kg pin move**, the 04D shape: the cut
landed in fe, passed its full standalone CI, and left kg on 05C by
design — the short window where fe spoke integers and kg did not was
exactly the plan's.  The cut: the Emacs number lexer replacing `strtod`
(`42` reads as an integer, `42.0` as a float, `0x10`/`inf`/`nan`/`1e`
are symbols again, `1.` is the integer 1, an overflowing literal reads
as a double); the printer's integral shortcut deleted so floats print
`42.0` in shortest-round-trip form and the exceptional values print
`1.0e+INF`/`-0.0e+NaN`; `eq`/`eql` landed as core primitives with the
pinned E-row semantics (`(eq 3 3)` → `t`, `(eq 3.0 3.0)` → `nil`,
`(eql 0.0 -0.0)` → `nil`); `FE_API_VERSION`/`FE_LANGUAGE_VERSION` 3 → 4
and `FeVersion` `"4.0"` → `"5.0"`; `scripts/mandelbrot.fe` migrated to
explicit float spellings so its 150 000-line golden stayed **byte-
identical** (the migration-preserved-program proof), and the full fe
workstream re-ran green: `make -C fe check` (three passes, goldens
unchanged), `compat` at **133 case(s), 111 passed, 22 known gap(s), 0
failed** — the long-pinned `reader-no-integers` flipped to `supported`
un-regenerated — and the nine-stage `.ci/run-ci-steps.sh`.  Actuals:
scc 530 → **538** (+8), `fe_eval.c` 332 → **330**, pmccabe 721 → **752**
(+31; the cut cost less than the tower, but the phase's pmccabe total
had already overspent its funding — see below).  `FeMinimumArenaSize()`
**56304 bytes**, kg 1 MiB arena **1097 frames / 56225 object slots** —
05A's projected numbers, verified exact.  Sub-plan 05E may start: it is
the pin.

**05E complete, 2026-08-05 — and with it Phase 5 and milestone 1.**
kg pin moved to fe `66ff904` — one commit past 05D's `17ac959`, added
minutes before the pin because 05D left `eq`/`eql` unclaimed in fe's
manifest and kg's `lisp-compat-check` is the only gate that notices;
that follow-up, not `17ac959`, closed the fe workstream, and the gate
gap it exposed is recorded in the post-close review below — with every
kg adaptation in **one atomic
commit** per Rule 10: `lisp_position()`'s funnel returns integers
(`point`, `point-min`/`point-max`, `line-number-at-pos`,
`current-column`, `char-after`, `string-length`, `string-to-char`, the
hook dispatch's `old_len`, and through the shared constructor all the
position/search/marker natives); `format`'s two gates widened (`%d`
prints an integer exactly and truncates a float — `87.6` → `87` — and
`%e`/`%f`/`%g` accept either type, as Emacs does; `%d` of NaN or an
infinity still refuses); `numberp` went two-tag; the prelude's `eq`
alias was deleted for fe's primitive and `equal`'s atom tail rewritten
over `string=`/`eql`/`eq`; `zerop` joined as a one-liner; both spelling
parsers and the compat manifest moved in step; and the enumerated
   expectation allowlist — five lines of `lisp-math-functions.yaml`, the
`(/ 1 0)`-family and `(type-of 1)`/`(format "%d" 9007199254740993)`
assertions in `test/test_lisp.c`, the `native-type-of` and
`format-exceptional-float` manifest flips — was the only text that
moved, each justified by a pinned oracle answer.  kg scc **5457/5500**
(+7 against the +15 to +25 row; no raise), pmccabe +1 on each of
`format_integer`, `format_float`, `native_numberp`, banked into the
per-symbol baseline in the same commit.  `make check`,
`make WITH_LISP=0 clean all check`, both complexity gates, `header-check`,
`docs-check`, `lisp-compat-check`, `lisp-prelude-check`, and
`JOBS=8 .ci/run-ci-steps.sh --parallel` are green from an idle tree.

### What the plan got wrong, collected

- **The price row was under-priced, and milestone 1 closed the caps.**
  Phase 5 priced at +50 to +70 landed at **+79 scc / +92 pmccabe**;
  against the funded raise (scc +60, pmccabe +70) the pmccabe funding
  was overspent by 22 points.  The tree closes milestone 1 at
  **scc 538/540, `fe_eval.c` 330/340, pmccabe 752/760** — two, ten and
  eight points of headroom, the reason the milestone-2 rows above are
  re-priced and the finding Phase 6's 06A Decision must fund against.
- **05C alone consumed the top of the phase row** (+70 scc); 05D was
  nearly free in scc (+8) but still cost +31 pmccabe — the cut was
  cheaper in the unit the row was priced in, and the tower was the whole
  row.  Per-slice actuals are above.
- **The arena projection was the rare exact prediction**: 05A's spike
  numbers (56304 B, 1097 frames / 56225 slots) verified identical at
  05D, and `FeMinimumArenaSize()`'s own invariant held (05B added no
  interned symbols and moved nothing).
- **The PTY count was off by one.**  05E and the audit text called the
  `lisp-math-functions.yaml` change set six lines, but its own enumeration
  names five (`sin`, `cos`, `sqrt`, `atan`, and `log`); those five changed
  and `expt` stayed `256` as required.
- **The prelude count arithmetic cancelled.**  05E said "count drops
  52 → 51" from deleting the `eq` alias, and separately added `zerop`;
  the two cancel and the tree landed at 52.  Both spelling parsers were
  already right at 52 and needed no edit — the plan's "move in step"
  happened by cancellation, and the disjointness tripwire (the real
  guard) fired as designed.
- **Everything else the plan named, held.**  Every oracle prediction
  from 05A through 05D, the exact-step pins, the `mandelbrot` golden
  byte-identical via script migration, `TestNumericTower` surviving 05D
  unedited, and the expectation allowlist's "an edit outside this list
  is a finding" rule — nothing outside it moved.

### Carried forward

- **The milestone-2 re-pricing this paragraph's parent promised is
  done, above.**  Phases 6–9 are re-priced against measured milestone-1
  comparables; the fe rows start from the reality that the caps are
  nearly full, so Phase 6's 06A Decision must raise all three fe caps —
  `SCC_COMPLEXITY_MAX` (2 points free, the tightest),
  `PMCCABE_TOTAL_MAX` and the `fe_eval.c` file cap — before any Phase 6
  implementation, and Phase 8's Wave A is priced as the size of
  Phase 4.
- **No `number-to-string`/`string-to-number`, no `min`/`max`/`abs`/
  `mod`, no `?a`, no radix syntax, no bignums** — 05A's standing
  exclusions; the first package that needs one is Phase 8's evidence.
- **`arith-error` and overflow are message-level** until Phase 6's
  condition objects, the standing rule.
- **No clamp-policy change**: `goto-char`'s clamping,
  `lisp_optional_count`'s saturation and `goto-line`'s bounds kept
  their shapes with integer inputs.
- **The five sub-plan documents (`05a`–`05e`) remain in the directory
  until the reviewer accepts the closed workstream**, per the set
  README's removal rule; this Status section and the commits are the
  record.

**Milestone 1 is closed.**  Phases 0–5 delivered the payoff §0.3
promised — an oracle-backed contract, a prelude that is Lisp source,
real `setq`/`set`, `=` as chained numeric equality, Lisp-2 namespaces,
and integers end to end, with `(= 3 3)` true and the phase's expected
text changes exactly the enumerated allowlist.  Milestone 2 (Phases
6–10) is now planned-but-unfunded with measured prices instead of
estimated ones, and the first obligation it carries is Phase 6's own
Decision to raise the caps it cannot otherwise fit in.

### Post-close review, 2026-08-05

Three independent adversarial reviews ran against the closed phase —
fe implementation, kg cutover, documentation/manifest truthfulness —
plus a differential sweep against the live oracle.  The kg cutover
held: no crash, no wrong result, every semantic answer byte-identical
to Emacs, +7 scc against a +15–25 row, the expectation edits exactly
the allowlist.  All 126 existing oracle snapshots regenerated
byte-identically, the float printer matched Emacs on 325 randomized
doubles, and the goldens the plan said must not move did not.  What
did not hold was, once again, the degenerate tail — and one behaviour
class 05A never asked the oracle about:

- **NaN through the new rounding family was undefined behaviour**
  (`IntegerRound`'s range guard passes NaN; UBSan-confirmed,
  silently `INT64_MIN`), reachable only from kg (Fex shadows the
  names in the standalone binary) and exercised by no gate; `(floor
  0 0)` — the divide-by-zero degenerate the Phase 4 lesson names —
  was silently wrong the same way.  Fixed in fe `c0e0ad8`: NaN and
  zero divisors raise `arith-error`, tested across all four functions
  and both operand types.
- **`1eINF`/`1eNaN` read as nonfinite floats**; Emacs requires the
  explicit `+` and reads them as symbols.  Fixed in fe `a86fe0d`.
- **Two comparator behaviours contradicted the oracle while their
  comments claimed to match it**: a single operand type-checked where
  Emacs answers `t` unconditionally, and chains did not short-circuit
  where Emacs stops at the first false pair (`(< 2 1 "a")` → `nil`
  there, an error here).  Fixed in fe `4b22170`/`e351b67` — deleting
  the checker paid for the whole review in scc, and six new oracle
  cases pin both shapes.
- **`is` silently lost its epsilon across int/float**, and the
  regression was papered over by editing `scripts/math.fe`'s
  assertion — the exact "a golden that moves is a finding, not a
  refresh" case 05C's own text warns about.  Fixed in fe `90b7b41`;
  the script edit is reverted and the golden byte-identical.
- **fe had no gate requiring primitives to be claimed in its own
  manifest** — which is why 05D shipped `eq`/`eql` unclaimed and
  `66ff904` had to patch it six minutes before kg's pin.
  `check_compat_manifest.py` now parses `primitive_names[]` itself
  (fe `edc3161`); the fuzz grammar gained int64 sentinels so the
  overflow arms it advertised are actually reachable (`1409c48`,
  coverage-verified); five compat rationales were rewritten to say
  what is true now (`881f6a0`), including `num-type-of`'s honest
  statement of the surviving `double`-vs-`float` spelling gap.
- **kg's side of the same honesty debt**: `native-type-of` had
  flipped to `supported` while `(type-of 1.0)` still answers `double`
  — the manifest's only record of that live divergence, deleted in
  the phase whose purpose was closing divergences; the editor still
  said `expected double, got string` at its argument seam
  (`lisp_finite` now raises `wrong-type-argument`, message-level);
  `auto-fill.el`'s header still claimed fe has no `>`; and a tail of
  stale figures (1098-vs-1097 frames, "42 primitives", the 05C arena
  misattribution in `doc/fe-upstream.md`, `eq` still listed as a
  prelude alias) disagreed between documents that were both rewritten
  by 05E.  Fixed in kg `c68c877` (the pin move to `e351b67` plus every
  behaviour-coupled adaptation: the seam, the manifest flip-back, the
  new tests that pin fe's NaN/comparator fixes from kg — the only host
  that reaches the unshadowed core) and `a8da121` (the documentation
  corrections), which also add the oracle-backed claims 05E promised
  but did not land (the widened `%e`-of-integer gate, `numberp`'s
  float tag, `equal`'s number leaves; kg manifest 135 → 138) and
  document the measured step-budget cost of 05E's `equal` atom-tail
  rewrite — re-measured during the fix at **20.0%** less reachable
  list length (7489 vs 9361 elements under the default budget),
  sharper than the review's ~15% estimate.

Cost of the review round: fe **−5 scc / −1 pmccabe** (533/540,
751/760 — the deleted comparator checker outweighed the new guards),
kg **+4 scc** (5461/5500; `lisp_finite`'s type gate, pmccabe 2 → 4
banked).  fe's nine-stage runner is green at `e351b67`; kg's full
parallel runner closed the round.  The lesson, recorded beside
Phase 4's: **"context reuse after error" degenerates are still not
enough — every behaviour a comment attributes to Emacs must have an
oracle case, because two of the four semantic defects lived in
comments confidently asserting the opposite of a measurement no one
had made.**

## Status — Phase 6

**06A complete, 2026-08-05.**  The completion target is pinned and the
phase is funded; no behaviour changed.  The audit's answer table landed as
**39 `planned` `cond-*` cases with fresh Emacs 31.0.90 snapshots** in
`fe/compat/` (the multi-measurement rows CC10/CT5/U1 expanded to one case
per answer, plus the three data-shape rows), every rationale naming Phase 6
and its implementing slice, and `make -C fe compat` is green (**181 cases,
119 passed, 62 known gaps, 0 failed**) with `compat-oracle` writing the 39
new snapshots and changing no existing one (**174 unchanged**).  The five
Decisions -- the condition object and static hierarchy, the fate of
`FeHandleError`'s 97 sites, condition-case scope with `:success` deferred,
the cleanup-raise policy as match-Emacs, and the host API / structured
channel -- and the funding Decision are recorded in the Decision section
above, with the three-cap raise **540 → 680 / 340 → 420 / 760 → 900** proved
live by the temporary-lowering trick on this slice's tree.  `fe/doc/
unwind-design.md` is corrected (the false "Emacs discards" claim, the
half-built enum and checkpointed drain, Phase 5's message-level residue,
the token/cancel registry pointer at Phase 9), and the parent plan's Phase
6 section carries the 06A reconciliation block.  `make -C fe check` (three
passes, goldens byte-identical), `complexity-check` and `pmccabe-check` at
the new caps, `format-check` and `compat` are all green.  The existing
`errors-and-non-local-exits` rows stay as they are (`signal-and-quit`
`planned` until 06D, `void-function-error`'s message-source note the thing
06D upgrades).  Sub-plan 06B may start.

**06B complete, 2026-08-05** (fe `229192e`, kg pin `539f19f`).
`FeCompletion` is public, `FeGetCompletion`/`FeGetCondition` are the
additive accessors, Quit/Budget are assigned at their producers (the
interrupt path and the three walls), the `CleanupFrameReserve`
coupling is asserted, and `TestCompletionKinds` runs.  Every golden
and message string byte-identical.  Cost: **+0 scc / +3 pmccabe** —
the "nearly free" prediction held.

**06C complete, 2026-08-05** (fe `0c0f8a0`, kg pin `b726bd4`).
`FeFrameCatch` carries the checkpoints, `PerformThrow` walks the
run's frames under the `run_base` wall, tags compare by `eq` per the
CT rows, `no-catch` raises through the error path,
`scripts/catch-throw.fe` and the fuzz builders exist, and the
containment divergence is recorded in `doc/fe-upstream.md`.  Cost:
**+12 scc / +13 pmccabe**.

**06D complete, 2026-08-05/06** (fe `c88ca0c` + `ae55024`).  The cut
itself: condition objects at raise time, the static hierarchy,
`signal`/`error`/`condition-case`, the cleanup-raise replacement
policy, the structured compat channel (`FE_STRUCTURED_ERRORS`),
`planned-quit-signal` flipped, versions API 5 / LANGUAGE 5 /
`FeVersion "6.0"`.  Message text byte-identical at every
pre-existing site.  Cost: **+89 scc / +74 pmccabe**, `fe_eval.c`
338 → 420 — landing exactly on its cap with zero headroom, which the
acceptance review then billed (below).  **What this slice shipped
short**, found by the review: no `condition-case` tests in
`test_api.c` at all (the parent's 5×3 gate matrix and the named
hook-shaped re-entry case included), the U-row compat cases
unrunnable as written, `(quit …)` handlers not actually catching a
real C-g, a caught condition disarming the evaluation-control record,
and Decision 4's cleanup-`throw` half unimplemented.

**06E complete, 2026-08-06** (kg `e0d874a`, pinning fe `1d96a58` after
the review-fix range).  The cutover in one atomic green commit:
quit/error/budget classified at every recovery seam (`C-g` shows
`Quit`; budget keeps its message; errors keep their formats),
`lisp_search`'s two hand-rolled quit producers converted to
`FeRaiseCompletion`, `ignore-errors` in the prelude (52 → 53, both
spelling parsers in step), the `errors-and-non-local-exits` manifest
category with its two honest `divergent` rows, the hook and process
seams rebuilt on `FeTryCallWithOptions` (quit/budget are re-signalled,
never contained), `save-excursion`/`with-current-buffer` transparent
to `condition-case` via `FeResignal`, two new PTY cases (408 total),
and the documentation family rewritten.  kg cost across the phase:
**+28 scc** (5461 → 5489/5500), against the funded +15..30 row.

### What the phase cost, collected

fe, baseline `e351b67` → close `1d96a58`: scc **533 → 654/680**
(+121, top of the funded +80..120 band), `fe_eval.c` **326 → 440**
against a file cap re-funded twice (340 → 420 at 06A, 420 → **460**
during the review fixes — 06A underfunded this one axis), pmccabe
**751 → 863/900** across 263 → 292 symbols, worst function
`DispatchPrimitive` 15/22.  `FeMinimumArenaSize()` 56304 → 56504
(06C) → 56824 (06D) → **56856** (review fixes); kg's 1 MiB arena
**1097 frames / 56225 slots** at the close.  fe compat **195 cases,
170 passed, 25 known gaps, 0 failed**; kg manifest 143 features →
148 with the new category, `lisp-compat-check` 0 problems, oracle
verify-by-running 60 cases 0 written/updated.

### What the plan got wrong, collected

- **06E's expectation discipline was self-contradictory**: "changes
  no existing message text" and "the quit seam reports `Quit`" cannot
  both hold — three existing assertions had to change, and the plan's
  own allowlist forbade it.  The Status rule going forward: an
  outcome that changes what a seam *reports* must enumerate the
  assertions it will edit, by name, in the plan.
- **The containment architecture was designed on a false premise.**
  06E assumed a host `setjmp` in a native could contain a nested
  run's raise; fe unwinds the native's C frame before `error_fn`
  runs, so the longjmp back is UB — the shipped first attempt
  corrupted the GC stack (and at the old baseline, segfaulted).  The
  fix had to be fe-side API (`FeTryCallWithOptions`/`FeResignal`),
  which no slice had planned; the unplanned `FeSaveEvalState` pair
  shipped mid-cutover was reverted as unworkable.
- **A slice can claim its cap headroom precisely and still be wrong**:
  06D landed on 420/420 exactly, so the first two-point addition
  (itself unplanned) broke the gate.
- **06A authored six oracle cases in a dialect fe cannot parse**
  (`progn`/`push`), leaving the U-family permanently unrunnable until
  the review rewrote them.
- The 06D row of `doc/fe-upstream.md` carried transcription errors
  (1088/56287 for kg's measured 1097/56225-ish partitioning), caught
  by re-measurement.

### Carried forward

- kg's editor natives still raise prose `(error "text")` — 06A
  Decision 2's deferral — so `condition-case-native-errors` is
  `divergent`: a `(wrong-type-argument …)` handler does not catch
  `(goto-char "x")`; a generic `(error …)` handler does.
- `catch`/`throw` do not cross `save-excursion`/
  `with-current-buffer` (the protected call's barrier is a catch
  wall; a throw becomes `no-catch` inside and re-signals as that
  error).  `catch-throw-reachability` is `divergent`; the recorded
  design for closing it — expanding both forms to Lisp
  `unwind-protect` over push/pop natives — is in `doc/TODO.md`.
- Call-trace exposure, prose-site classification, and the
  token/cancel cleanup registry remain deferred (TODO, Phase 9
  pointers).
- kg's scc gate closes Phase 6 at **5489/5500**; 07A's funding
  Decision is the recorded consumer of that fact.

### The acceptance review, 2026-08-06

This phase's review ran *before* acceptance and was load-bearing: the
tree as first delivered was red (kg's `make check` failed on the new
hook-containment test; fe's `complexity-check` and `format-check`
failed; one intermediate kg pin commit did not compile).  Three
adversarial reviews plus a Phase 7 fact audit produced ~40 findings,
the worst being semantic: a caught condition permanently disarmed the
step limit, frame wall and C-g interrupt (an `ignore-errors` in an
init file left the editor unboundable and un-interruptible); a real
C-g was not catchable by a `(quit …)` handler and its catchability
depended on stale state; quit/budget inside a cleanup reached the
host as Error; a cleanup's throw could never find any catch and
escaped the catch's own tag as `no-catch`; and the `error`
primitive's format directives diverged from Emacs with a golden
pinning the wrong answer.  The fe fixes are `374e52f`..`1d96a58`
(12 commits, nine-stage runner green); the kg cutover was rebuilt
from scratch as `e0d874a` (the original cutover commit and the
non-compiling pin-only commit `8e3e925` were removed from the branch
— Rule 10 now holds at every commit).  Final green light:
`JOBS=8 .ci/run-ci-steps.sh --parallel` **12/12 PASS** on kg, all
nine fe stages green, 32 native / 408 PTY, `WITH_LISP=0` green.
Lessons banked: a containment seam is fe's to own, never a host
`setjmp`; a plan whose Outcome changes a seam's report must
enumerate the assertion edits it implies; and landing exactly on a
cap is a finding, not a success.

## Status — Phase 7

Complete, 2026-08-06.  Five slices as planned, then an acceptance
review that **rejected the delivery on both sides of the pin despite
every gate being green**, and a two-repository fix cycle that closed
it for real.  The implementation commits: 07A `abfc384` (kg) +
`7452143` (fe), 07B `035614d` (fe), 07C `f7955b7`, 07D `718157f`,
07E `d062e63`.  The fix cycle: fe `b6bf07d`..`e576a0b` (11 commits),
kg `68ad2f0`..`7468c9a` (8 commits, including the pin move to
`e576a0b`).

What shipped, at close: unconditional strict arity with
`wrong-number-of-arguments` `(FUNCTION NARGS)` data and a full
53-primitive arity table (`FeSetStrictArity`/`-a` removed without
residue); `(interactive …)` as real command metadata with the
declaration recognised only in Emacs' position; argument construction
for nil/`p`/`P`/`r`/FORM and the prompting letters
`n`/`N`/`s`/`f`/`F`/`b`/`B` behind one seam with cancel-is-quit and
overflow-is-error; the raw prefix (`nil`, integers, `(4)`/`(16)`/…,
`-`) delivered as a command-boundary `current-prefix-arg` binding;
nested `(command-execute …)` executing inside the active evaluator via
`FeTryCallWithOptions`/`FeResignal` and returning the command's value;
`editor_read_line_path` returning `enum minibuf_result` end-to-end;
and `FE_API_VERSION`/`FE_LANGUAGE_VERSION` 6, `FeVersion` "7.0".

### Per-slice actuals

- **07A** — the oracle tables and funding landed as written; two of
  its snapshot families were later falsified by 07B and restored by
  the review (below).  Funded fe 680→760/460→520/900→980 and kg
  5500→5660; the kg figure it argued from (5489) was the true floor.
- **07B** (`035614d`) — scc 654→694, and the review found the
  delivery's worst defects here: an argument-list copy no plan asked
  for overflowed the GC root stack (~1400 args → SIGSEGV of the
  embedded interpreter, invisible to nine green CI stages), and four
  oracle snapshots were hand-edited to fe's own printer output with
  two more cases rewritten (`progn`→`do`) under retained snapshots —
  the same falsification mode Phase 6's review caught, despite 07A
  Decision 2 prescribing the correct handling in advance.  Also:
  improper lists misrouted into the arity path with `FUNCTION`=nil,
  `apply` minimum 2 and `lambda` requiring a body against both the
  census and Emacs, a doc example that raised, three owed test
  families absent, an uncommented nine-primitive message-policy
  split, silent upward pmccabe re-banks, and a fuzz seed claiming
  coverage it measured at zero of 24 buckets.
- **07C** (`f7955b7`) — **+0 scc, the no-adaptation bet held**: the
  only `src/` change is the two version asserts, no expectation
  edited, suites green across the pin.  The one honest slice of the
  delivery.
- **07D** (`718157f`) — +89 scc (priced +80..120).  Semantically the
  headline was dead on arrival: `raw_kind` was zeroed on the
  prefix-commit path, so `C-u 5 M-x cmd` passed 1 to every Lisp
  command — none of the slice's eight enumerated PTY cases existed to
  catch it.  Nested `command-execute` aborted the editor (unbalanced
  global recovery frame).  A lone-docstring `defun` regressed against
  Emacs *with the existing test edited to the wrong answer*, and
  redefinition ran defalias-before-registration, inverting the plan's
  atomicity ordering.
- **07E** (`d062e63`) — +78 scc against a +30..50 price, the phase's
  one materially missed estimate.  The prompting codes and `f`/`F`/
  `b`/`B` contracts were faithful (the review verified all six
  `buf_read_name` sub-clauses live), but the `bufmgr.c` carve-out —
  contracted behaviour-preserving — shipped four regressions
  (mid-line erase cursor snap; `C-x b` RET no longer dismissing;
  sticky overflow making `C-x b` and path prompts permanently inert;
  M-RET as an undocumented new accept), the numeric prompt accepted
  `""`→0/`inf`/`0x10` via raw `strtod`, multi-clause prompts leaked
  the rest of the spec into the minibuffer, and the coverage baseline
  was regenerated over three per-file floor regressions in an
  empty-bodied commit.  Zero of the slice's owed native tests
  existed.

### The acceptance review, 2026-08-06

Three adversarial reviews (fe, kg, docs) plus the Phase 8 fact audit,
all against the pinned Emacs.  Verdicts: fe **not landable** (2
critical, 5 major, 12 minor), kg **reject** (2 critical, 7 high, 7
medium, 11 low), docs 15 findings (2 severe).  The common cause,
named by every reviewer: **the tests each sub-plan enumerated were
not written** — 07D shipped zero of its PTY obligations, 07E zero of
its native ones, and every green gate measured something other than
the new semantics.  A second systemic hole: kg's PTY harness only
checked exit status for cases that declared `expected_exit_code`, so
ASan/LSan reports in the lanes never failed CI.

The fe fixes (`b6bf07d`..`e576a0b`) closed all 19 findings — GC-root
reserve + allocation-free overflow raise, snapshots restored from
`7452143` with NARGS extracted Lisp-side and the rendering divergence
recorded, census minimums corrected, uniform primitive arity policy,
the owed test families (including a re-entering native and a
forced-GC sweep), honest fuzz seeds with reachability counts, honest
pmccabe re-banking with a new refuse-silent-increases guard — and
surfaced three pre-existing bugs: `RaiseCondition` could produce an
*uncatchable* condition under memory pressure (a handler naming the
right condition missed it), `ResumeEvalList` had the same per-operand
GC-stack growth (`(list 1 … 4000)` overflowed on the old base too),
and `((lambda (x)) 1)` answered `(1)` via a stale accumulator.  fe
closed at scc **670/760**, `fe_eval.c` **453/520**, pmccabe
**886/980** across 299 symbols — the fix cycle *deleted* more than
the arity checks had added.

The kg fixes (`68ad2f0`..`7468c9a`) closed all 28 + 6 doc findings:
prefix delivery restored (and two further latent defects found by the
newly-written cases — `M--` never reached kg at all because `'-'` was
missing from the tty meta-key table, and its effective value was 4,
not −1, via a dead ternary arm); the nested path rebuilt frameless on
`FeTryCallWithOptions`; the docstring and atomicity regressions
reverted to Emacs' answers; a real numeric-token classifier; the four
`bufmgr.c` regressions undone behind an explicit `buf_name_mode`
enum; FORM arguments rooted; the leak class fixed *after* arming the
PTY harness to require clean exits on every case (which turned ci-04
red on three real leaks, including a prefix-binding stack the review
had missed); the hand-written oracle snapshot deleted and an
orphan-snapshot check added to `check_lisp_compat.py`; `commandp`
implemented; the manifest and interactive docs made truthful
(including the man page's roff-broken code list); and coverage
recovered above the pre-07E floors before re-banking.  kg closed at
**5714/5730** (cap re-set at its measured actual), **32 native / 427
PTY** (11 new cases), `WITH_LISP=0` **341 pass / 86 skip**, coverage
**86.0% lines / 96.9% functions / 74.1% branches** — all three totals
above the delivery's.

### What the plan got wrong, collected

- **Enumerated tests are acceptance criteria, and nothing enforced
  them.**  Both kg slices shipped with their test sections unmet and
  every gate green; the four worst defects were each one prescribed
  case away from impossible.  The handoff-contract table now exists
  precisely to be checked row-by-row at review, and the review did —
  but only the review.  A future set should make the owed-test list
  mechanically checkable where it can be (case names in the plan
  grep-able against the tree).
- **07E's price (+30..50) missed by 56%** — the `bufmgr.c` extraction
  was priced as a carve-out and executed as a rewrite.
- **The close note carried figures forward** (5649 for a measured
  5656, 73 skips for 79, the previous pin's arena numbers) — Rule 6
  violated in the phase's own record, in a note filed inside Phase
  6's Status; this section replaces it.
- **Snapshot falsification recurred** despite the Phase 6 lesson and
  a Decision prescribing the correct mechanism.  The countermeasure
  is now partly structural: fe's baseline tool refuses silent
  increases, kg's compat checker refuses orphan snapshots, and an
  edit to an existing snapshot in an implementation commit is a
  reviewable red flag by convention.
- **An unplanned semantic addition in a "checks-only" slice**
  (07B's argument-list copy) caused the worst crash of the program to
  date.  The 06E lesson generalises: a slice whose contract is
  "checks and raises" must not also change evaluation order or data
  flow unannounced.

### Carried forward

- Deferred interactive codes (`d m S x X` …) and modifiers
  (`* @ ^` — now reported "unsupported", not "invalid"), prompt `%`
  interpolation, `interactive` MODES (ignored, recorded), reflection
  (`interactive-form`/`func-arity`/`documentation` for commands —
  the stored docstring is still write-only), keyboard-macro strings,
  `CMD_REPEATS`.
- The `read-*` family (`read-string`, `read-number`,
  `read-file-name`, `read-buffer`, `y-or-n-p`) — the seam exists;
  the public functions wait for Phase 8+ corpus evidence.
- The 16-argument `LISP_INTERACTIVE_MAX_ARGS` bound; the
  `(FUNCTION NARGS)` rendering divergence (fe prints
  `(lambda …)`/`(quote …)` where Emacs prints `#[…]`/`'…`), and fe
  reporting the defalias'd symbol where Emacs reports the closure —
  all recorded divergence rows now.
- An improper argument list to a *closure* still raises fe's prose
  "dotted pair in argument list" where Emacs says
  `(wrong-type-argument listp TAIL)` — pinned with a test, worth a
  future row (the fe fix cycle's one recorded deferral).
- kg's scc gate closes Phase 7 at **5714/5730**; 08A's funding
  Decision is the recorded consumer of that fact.

The full parallel runner then earned its Rule 9 place by catching
three more items the fix cycle's own gate list had not covered:
clang-tidy's branch-clone check rejected the identical switch bodies
fe's dead-code removal left behind (fe `a14b43b`, a
disassembly-verified zero-behaviour merge), the stub
`editor_path_split` wrote neither of its outputs so the new prompt
tests ran `strlen` over uninitialized stack in the valgrind and MSan
lanes (plain and ASan builds had passed on stack luck), and an IWYU
cleanup mid-close deleted the unconditional `def.h` include the
`WITH_LISP=0` classifier build needs (kg `4ffc801`, `e57043b`,
`9697f6e`).  Final green light: `JOBS=8 .ci/run-ci-steps.sh
--parallel` **12/12 PASS** on kg at `9697f6e` with the pin at
`a14b43b`, all nine fe stages green standalone.
